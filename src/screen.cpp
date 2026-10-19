#include "screen.h"

#include <math.h>

#include <initializer_list>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_timer.h"
#include "font_ui.h"
#include "qrcode.h"

namespace screen {

// classic 5x7 font, ASCII 32..126, 5 columns per glyph, bit 0 = top row
// ---- text in the brand font (Quicksand, tools/fontgen.py): laid out once into a small alpha image when a page
// is set, per pixel only looked up
struct TextImg {
  int16_t w, h;
  uint8_t a[240 * 40];  // alpha 0..15, row by row
};
enum { S_STATUS, S_TILE0, S_ASKT = S_TILE0 + 4, S_ASKL0, S_ASKL1, S_YES, S_NO, S_SPL_T, S_SPL_T2, S_SPL_L, S_SHOW0,
       S_N = S_SHOW0 + 4 };
EXT_RAM_BSS_ATTR static TextImg txt[S_N];

static uint32_t utf8(const char*& s) {
  uint32_t c = (unsigned char)*s++;
  int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
  if (more) c &= 0x3F >> more;
  while (more-- && ((unsigned char)*s & 0xC0) == 0x80) c = c << 6 | ((unsigned char)*s++ & 0x3F);
  return c;
}
static const UiGlyph* glyph(const UiFont& f, uint32_t cp) {
  for (int i = 0; i < f.n; ++i)
    if (f.g[i].cp == cp) return &f.g[i];
  return glyph(f, '?');  // ('?' is in every font)
}
static void layout(TextImg& t, const UiFont& f, const char* s) {
  int w = 0;
  for (const char* p = s; *p;) w += glyph(f, utf8(p))->adv;
  t.w = w < 240 ? w : 240;
  t.h = f.height < 40 ? f.height : 40;
  memset(t.a, 0, t.w * t.h);
  int pen = 0;
  for (const char* p = s; *p;) {
    const UiGlyph* g = glyph(f, utf8(p));
    for (int y = 0; y < g->h; ++y)
      for (int x = 0; x < g->w; ++x) {
        int X = pen + g->x + x, Y = g->y + y, i = y * g->w + x;
        if (X < 0 || X >= t.w || Y < 0 || Y >= t.h) continue;
        uint8_t b = f.data[g->off + i / 2], v = i & 1 ? b & 15 : b >> 4;
        if (v > t.a[Y * t.w + X]) t.a[Y * t.w + X] = v;
      }
    pen += g->adv;
  }
}
// how much of a laid out text covers (x, y): centred on mid, its line starting at top
static inline float textA(const TextImg& t, int mid, int top, int x, int y) {
  int l = mid - t.w / 2;
  if (x < l || x >= l + t.w || y < top || y >= top + t.h) return 0.f;
  return t.a[(y - top) * t.w + (x - l)] * (1.f / 15.f);
}

static volatile bool on = false;
static bool isMenu = false, isSplash = false, isBrand = false;
static volatile int splashPct = -1;
static volatile uint32_t ver = 0;
EXT_RAM_BSS_ATTR static uint8_t qr[64][64];  // modules (version <= 10 -> 57 x 57)
static int qrN = 0;

static void grab(esp_qrcode_handle_t q) {
  qrN = esp_qrcode_get_size(q);
  if (qrN > 64) qrN = 0;
  for (int y = 0; y < qrN; ++y)
    for (int x = 0; x < qrN; ++x) qr[y][x] = esp_qrcode_get_module(q, x, y);
}

void show(const char* title, const char* code, const char* l1, const char* l2, const char* l3) {
  isMenu = isSplash = false;
  layout(txt[S_SHOW0], F_LABEL, title);
  layout(txt[S_SHOW0 + 1], F_TEXT, l1);
  layout(txt[S_SHOW0 + 2], F_TEXT, l2);
  layout(txt[S_SHOW0 + 3], F_TEXT, l3);
  esp_qrcode_config_t c = ESP_QRCODE_CONFIG_DEFAULT();
  c.display_func = grab;
  c.max_qrcode_version = 10;
  c.qrcode_ecc_level = ESP_QRCODE_ECC_LOW;
  qrN = 0;
  esp_qrcode_generate(&c, code);
  on = true;
  ver = ver + 1;
}
static void splashOn(int pct) {
  splashPct = pct;
  qrN = 0;
  isMenu = false;
  isSplash = true;
  on = true;
  ver = ver + 1;
}
void splash(const char* title, const char* line, int pct) {
  layout(txt[S_SPL_T], F_TITLE, title);
  layout(txt[S_SPL_L], F_TEXT, line);
  isBrand = false;
  splashOn(pct);
}
void brand(const char* line) {
  layout(txt[S_SPL_T], F_TITLE, "pixel");
  layout(txt[S_SPL_T2], F_TITLE, "buddy");
  layout(txt[S_SPL_L], F_TEXT, line);
  isBrand = true;
  splashOn(-1);
}
void hide() {
  on = false;
  ver = ver + 1;
}
bool active() { return on; }
uint32_t version() { return ver; }

// the setup page: title at the top, QR code in the middle, three text lines below
static const int TITLE_Y = 3, QR_TOP = 30, QR_BOX = 150, LINE1_Y = 183, LINE2_Y = 201, LINE3_Y = 219;

static inline uint16_t px(uint8_t r, uint8_t g, uint8_t b) {
  uint16_t v = (uint16_t)((r & 0xF8) << 8 | (g & 0xFC) << 3 | b >> 3);
#ifdef EYES_SWAP_BYTES
  v = (uint16_t)(v >> 8 | v << 8);
#endif
  return v;
}

// brand colours (docs/brand.md)
static const uint8_t CREAM[3] = {246, 243, 234}, BLUE[3] = {61, 107, 255}, MUTED[3] = {139, 148, 169},
                     TRACK[3] = {37, 43, 58}, RED[3] = {255, 69, 58};
static inline float clamp01(float v) { return v < 0.f ? 0.f : v > 1.f ? 1.f : v; }
// rounded box (pill), signed distance
static inline float sdPill(float x, float y, float hx, float hy) {
  float r = hx < hy ? hx : hy, qx = fabsf(x) - hx + r, qy = fabsf(y) - hy + r;
  float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
  return sqrtf(ox * ox + oy * oy) + (qx > qy ? (qx < 0 ? qx : 0) : (qy < 0 ? qy : 0)) - r;
}
static inline float easeOut(float t) {
  t = clamp01(t);
  return 1.f - (1.f - t) * (1.f - t) * (1.f - t);
}
static inline void blend(float* c, const uint8_t* v, float a) {
  for (int k = 0; k < 3; ++k) c[k] += (v[k] - c[k]) * a;
}
// ------------------------------------------------------------------ the menu (BOOT)
// Up to four big tiles (2 x 2, each a whole quarter of the screen to hit), or a question with No / Yes:
// nothing happens on a single stray touch. Small Pixel Buddy eyes on top watch the finger and blink.
// Pages pop in (tiles one after the other, with a little overshoot); a pressed tile sinks in and acts
// when the finger lets go on it. Flat brand colours (docs/brand.md).
static const uint8_t NIGHT[3] = {11, 13, 18}, CARD[3] = {24, 28, 40}, CARD_HI[3] = {40, 47, 66};
static const int ASK_Y = 172;  // a question: the icon and the text above, the buttons below
struct MTile {
  int icon;
  uint8_t col[3];
};
static MTile tiles[4];
static int nTiles = 0;
static bool asking = false, askOne = false;  // a question; with a single OK button
static int askIcon = 0, eyeMood = 0;         // eyes: 0 curious, 1 worried, 2 sleepy
static uint8_t askCol[3], chipCol[3], yesText[3];
static volatile int pressedI = -1;
static bool micOff = false;  // the microphone button in the header: muted
static int64_t openT0 = 0, pageT0 = 0, blinkT0 = 0, nextBlink = 0, lookUntil = 0;
static float tPage = 9.f, tOpen = 9.f, blinkK = 1.f;  // taken once per frame: all bands alike
static float lookX = 0.f, lookY = 0.f, lookTX = 0.f, lookTY = 0.f;
static float popA[5], popK[5], popInv[5];  // per tile / button this frame: alpha and scale (the pop-in, pressed); 4 the text
static float drawnA[5], drawnK[5], drawnHa = -1.f, drawnBg = -1.f;  // as last drawn: unchanged ones are skipped
static int drawnPressed = -2;
static volatile int pageGen = 1;  // the page image below (pageRow) is redrawn when this changes
EXT_RAM_BSS_ATTR static int rowGen[240];  // (internal RAM is short: the SD card needs it)
static inline float easeBack(float t);

static void rgb(uint32_t c, uint8_t* o) {
  o[0] = c >> 16 & 255;
  o[1] = c >> 8 & 255;
  o[2] = c & 255;
}
static void page() {
  int64_t now = esp_timer_get_time();
  if (!(on && isMenu)) openT0 = now;  // opened just now: the background and the eyes come up too
  pageT0 = now;
  pageGen = pageGen + 1;  // the page image is drawn again in the next frame
  drawnBg = -1.f;  // all of it once
  pressedI = -1;
  qrN = 0;
  isSplash = false;
  isMenu = true;
  on = true;
  ver = ver + 1;
}
void menu(const char* st, const char* const* labels, const int* icons, const uint32_t* colors, int n) {
  layout(txt[S_STATUS], F_SMALL, st);
  nTiles = n < 4 ? n : 4;
  for (int i = 0; i < nTiles; ++i) {
    layout(txt[S_TILE0 + i], F_LABEL, labels[i]);
    tiles[i].icon = icons[i];
    rgb(colors[i], tiles[i].col);
  }
  asking = false;
  eyeMood = 0;
  page();
}
void ask(const char* st, int icon, uint32_t color, const char* title, const char* line, const char* yes,
         const char* no, int mood) {
  layout(txt[S_STATUS], F_SMALL, st);
  askIcon = icon;
  rgb(color, askCol);
  for (int k = 0; k < 3; ++k) chipCol[k] = (uint8_t)(NIGHT[k] + (askCol[k] - NIGHT[k]) * 0.2f);  // flat tint
  bool light = askCol[0] * 0.3f + askCol[1] * 0.59f + askCol[2] * 0.11f > 140.f;
  memcpy(yesText, light ? NIGHT : CREAM, 3);
  layout(txt[S_ASKT], F_LABEL, title);
  const char* nl = strchr(line, '\n');  // up to two lines
  char l0[64];
  snprintf(l0, sizeof(l0), "%.*s", nl ? (int)(nl - line) : (int)strlen(line), line);
  layout(txt[S_ASKL0], F_TEXT, l0);
  layout(txt[S_ASKL1], F_TEXT, nl ? nl + 1 : "");
  askOne = !yes;
  layout(txt[S_YES], F_LABEL, yes ? yes : no);  // just one button: it says "no" (OK, back)
  layout(txt[S_NO], F_LABEL, no);
  eyeMood = mood;
  asking = true;
  page();
}
int hit(int x, int y) {
  if (!on || !isMenu) return -1;
  if (y < 54) return x >= 190 ? HIT_CLOSE : x < 50 ? HIT_MIC : x >= 86 && x < 156 ? HIT_EYES : -1;
  if (!asking) {
    int i = (x >= 120) + 2 * (y >= 148);
    return i < nTiles ? i : -1;
  }
  if (y < ASK_Y) return -1;
  return askOne ? 0 : x >= 120;
}
void press(int i) { pressedI = i; }
void micMuted(bool m) {
  micOff = m;
  drawnHa = -1.f;  // the header again
}
void lookAt(int x, int y) {
  lookTX = fmaxf(-1.f, fminf(1.f, (x - 120) / 90.f));
  lookTY = fmaxf(-1.f, fminf(1.f, (y - 23) / 110.f));
  lookUntil = esp_timer_get_time() + 1500000;
}
void poke() { nextBlink = 0; }
bool animating(int& x0, int& y0, int& x1, int& y1) {
  if (!on || !isMenu) return false;
  int64_t now = esp_timer_get_time();
  bool ready = true;  // the pop-in starts once the page image is complete (each row of this page)
  for (int y = 54; y < 240 && ready; ++y) ready = rowGen[y] == pageGen;
  if (!ready) pageT0 = now;
  tOpen = (now - openT0) * 1e-6f;
  tPage = (now - pageT0) * 1e-6f;
  if (now >= nextBlink) {  // a blink every 2.5 .. 5 s
    blinkT0 = now;
    nextBlink = now + 2500000 + (now / 1000 * 7919) % 2500000;
  }
  float b = (now - blinkT0) / 160000.f;
  blinkK = fminf(b < 1.f ? fabsf(1.f - 2.f * b) : 1.f, easeOut((tOpen - 0.05f) / 0.3f));  // and they open first
  if (now > lookUntil) {  // little glances around, a new spot every 1.6 s
    unsigned s = (unsigned)(now / 1600000) * 2654435761u;
    lookTX = (int)(s >> 8 & 255) / 160.f - 0.8f;
    lookTY = (int)(s >> 16 & 255) / 320.f - 0.3f;
  }
  lookX += (lookTX - lookX) * 0.3f;
  lookY += (lookTY - lookY) * 0.3f;
  for (int i = 0; i < 4; ++i) {  // the tiles (or No / Yes) pop in one after the other
    float st = asking ? tPage - 0.14f - i * 0.05f : tPage - 0.06f - i * 0.055f;
    popA[i] = clamp01(st / 0.18f);
    popK[i] = (0.8f + 0.2f * easeBack(st / 0.36f)) * (pressedI == i ? 0.94f : 1.f);
  }
  float st = tPage - 0.06f;  // the question: the icon pops (scale), the text fades in (alpha)
  popA[4] = clamp01(st / 0.18f);
  popK[4] = 0.7f + 0.3f * easeBack(st / 0.4f);
  for (int i = 0; i < 5; ++i) popInv[i] = 1.f / popK[i];  // (no float division per pixel: slow on this chip)
  // what to draw: what changed since the last frame (the background, the header, the items), the eyes always
  x0 = 88, y0 = 6, x1 = 152, y1 = 40;
  auto add = [&](int a0, int b0, int a1, int b1) {
    x0 = a0 < x0 ? a0 : x0, y0 = b0 < y0 ? b0 : y0, x1 = a1 > x1 ? a1 : x1, y1 = b1 > y1 ? b1 : y1;
  };
  float bgk = easeOut(tOpen / 0.15f), ha = easeOut((tOpen - 0.05f) / 0.25f);
  if (bgk != drawnBg) add(0, 0, 240, 240);
  bool hdr = pressedI == HIT_CLOSE || drawnPressed == HIT_CLOSE || pressedI == HIT_MIC || drawnPressed == HIT_MIC;
  if (ha != drawnHa || hdr) add(0, 0, 240, 54);
  for (int i = 0; i < 5; ++i) {
    bool moved = popA[i] != drawnA[i] || popK[i] != drawnK[i] || (pressedI != drawnPressed && (i == pressedI || i == drawnPressed));
    if (!moved) continue;
    if (i == 4) add(0, 54, 240, asking ? ASK_Y : 54);
    else if (!asking) add(i & 1 ? 120 : 0, i & 2 ? 148 : 54, i & 1 ? 240 : 120, i & 2 ? 240 : 148);
    else add(askOne || i == 0 ? 0 : 120, ASK_Y, askOne || i == 1 ? 240 : 120, 240);
  }
  memcpy(drawnA, popA, sizeof(popA));
  memcpy(drawnK, popK, sizeof(popK));
  drawnHa = ha, drawnBg = bgk;
  drawnPressed = pressedI;
  return true;
}

static inline float easeBack(float t) {  // ease out with a little overshoot
  t = clamp01(t);
  const float c = 1.6f;
  float u = t - 1.f;
  return 1.f + (c + 1.f) * u * u * u + c * u * u;
}
static inline float sdBox(float x, float y, float hx, float hy, float r) {
  float qx = fabsf(x) - hx + r, qy = fabsf(y) - hy + r;
  float ox = qx > 0.f ? qx : 0.f, oy = qy > 0.f ? qy : 0.f;
  return sqrtf(ox * ox + oy * oy) + fminf(fmaxf(qx, qy), 0.f) - r;
}
static inline float sdSeg(float x, float y, float ax, float ay, float bx, float by) {
  float px = x - ax, py = y - ay, dx = bx - ax, dy = by - ay;
  float h = clamp01((px * dx + py * dy) / (dx * dx + dy * dy));
  px -= dx * h;
  py -= dy * h;
  return sqrtf(px * px + py * py);
}
// an arc of radius R around (0, 0), open towards the unit direction (gx, gy) by the half angle g
static float sdArc(float x, float y, float R, float gx, float gy, float g) {
  float u = -(x * gx + y * gy), v = x * gy - y * gx;  // the gap at angle pi
  if (fabsf(atan2f(v, u)) <= (float)M_PI - g) return fabsf(sqrtf(x * x + y * y) - R);
  float ex = -R * cosf(g), ey = R * sinf(g);
  return fminf(sqrtf((u - ex) * (u - ex) + (v - ey) * (v - ey)), sqrtf((u - ex) * (u - ex) + (v + ey) * (v + ey)));
}
// line icons, 2.8 px strokes, about 26 px tall, centred on (0, 0): signed distance
static float icon(int k, float x, float y) {
  if (fabsf(x) > 16.f || fabsf(y) > 16.f) return 9.f;
  float d = 99.f;
  auto seg = [&](float ax, float ay, float bx, float by) { d = fminf(d, sdSeg(x, y, ax, ay, bx, by)); };
  switch (k) {
    case IC_RESTART:  // a circle arrow, clockwise
      d = sdArc(x, y, 10.f, 0.6f, -0.8f, 0.55f);
      seg(-3.f, -14.5f, 1.8f, -10.f);
      seg(-3.f, -5.5f, 1.8f, -10.f);
      break;
    case IC_HOTSPOT:  // radio waves
      d = fminf(sdArc(x, y - 7.f, 7.f, 0.f, 1.f, 2.3f), sdArc(x, y - 7.f, 13.f, 0.f, 1.f, 2.3f));
      d = fminf(d, sqrtf(x * x + (y - 7.f) * (y - 7.f)) - 1.f);
      break;
    case IC_HOME:
      seg(-12.f, 0.f, 0.f, -11.f);
      seg(0.f, -11.f, 12.f, 0.f);
      seg(-8.f, -3.f, -8.f, 11.f);
      seg(-8.f, 11.f, 8.f, 11.f);
      seg(8.f, 11.f, 8.f, -3.f);
      seg(-2.5f, 11.f, -2.5f, 5.f);
      seg(-2.5f, 5.f, 2.5f, 5.f);
      seg(2.5f, 5.f, 2.5f, 11.f);
      break;
    case IC_POWER:
      d = sdArc(x, y + 1.f, 10.f, 0.f, -1.f, 0.75f);
      seg(0.f, -13.f, 0.f, -3.f);
      break;
    case IC_RESET:  // a bin
      seg(-11.f, -7.f, 11.f, -7.f);
      seg(-3.5f, -11.f, 3.5f, -11.f);
      seg(-3.5f, -11.f, -3.5f, -7.f);
      seg(3.5f, -11.f, 3.5f, -7.f);
      seg(-8.f, -3.f, -6.5f, 11.f);
      seg(-6.5f, 11.f, 6.5f, 11.f);
      seg(6.5f, 11.f, 8.f, -3.f);
      seg(-2.5f, 0.f, -2.5f, 7.f);
      seg(2.5f, 0.f, 2.5f, 7.f);
      break;
    case IC_CLOSE:
      seg(-5.f, -5.f, 5.f, 5.f);
      seg(-5.f, 5.f, 5.f, -5.f);
      break;
    case IC_MIC:  // a microphone: the capsule, its holder, the stand
    case IC_MUTED:  // crossed out
      d = fabsf(sdPill(x, y + 6.f, 4.5f, 7.5f));
      d = fminf(d, sdArc(x, y + 4.f, 9.f, 0.f, -1.f, 1.2f));
      seg(0.f, 5.f, 0.f, 11.f);
      seg(-5.f, 11.f, 5.f, 11.f);
      if (k == IC_MUTED) seg(-11.f, -12.f, 11.f, 12.f);
      break;
  }
  return d - 1.4f;
}
// the small Pixel Buddy eyes in the header (like the logo): look around, blink, worry, get sleepy
static void eyesPixel(float x, float y, float* c, float al) {
  for (int i = 0; i < 2; ++i) {
    const float s = i ? 1.07f : 1.f, is = i ? 1.f / 1.07f : 1.f, cx = 120.f + (i ? 16.f : -16.f), cy = 23.f - (i ? 1.f : 0.f);
    float px = (x - cx) * is, py = (y - cy) * is;
    float hh = fmaxf(1.2f, 12.f * blinkK);
    float a = clamp01(0.5f - sdPill(px, py, 9.f, hh) * s) * al;
    if (a <= 0.f) continue;
    float side = i ? 1.f : -1.f;  // towards the outer corner
    if (eyeMood == 1) a *= clamp01(py - (-hh + 6.f + 0.45f * px * side) + 0.5f);  // worried: the outer lid lower
    if (eyeMood == 2) a *= clamp01(py - (-hh + 0.9f * hh) + 0.5f);                 // sleepy: half closed
    float ix = px - lookX * 3.5f, iy = py - lookY * 4.f - 1.f;
    float ai = clamp01((1.f - sqrtf(ix * ix / 30.f + iy * iy / 56.f)) * 6.f);
    float ah = clamp01(2.2f - sqrtf((ix + 1.8f) * (ix + 1.8f) + (iy + 3.f) * (iy + 3.f))) * ai;
    float v[3];
    for (int k = 0; k < 3; ++k) {
      v[k] = CREAM[k] + (BLUE[k] - CREAM[k]) * ai;
      v[k] += (255.f - v[k]) * ah;
      c[k] += (v[k] - c[k]) * a;
    }
  }
}
// a rounded button / tile that pops in at time st (seconds), sinks in when pressed; label below the icon
static void tilePixel(float x, float y, float* c, float al, float k, float inv, bool pressed, float cx, float cy,
                          float hw, float hh, const uint8_t* fill, int ic, const uint8_t* icCol, float icY,
                          const TextImg& label, const uint8_t* txCol, int txY) {
  if (al <= 0.f) return;
  float sx = cx + (x - cx) * inv, sy = cy + (y - cy) * inv;
  float dx = fabsf(sx - cx), dy = fabsf(sy - cy);
  if (dx > hw + 1.f || dy > hh + 1.f) return;
  // inside, away from the round corners: no distance needed
  float a = (dx < hw - 1.f && dy < hh - 18.f) || (dy < hh - 1.f && dx < hw - 18.f)
                ? al
                : clamp01(0.5f - sdBox(sx - cx, sy - cy, hw, hh, 18.f) * k) * al;
  if (a <= 0.f) return;
  uint8_t f[3];
  for (int q = 0; q < 3; ++q) f[q] = pressed ? (uint8_t)(fill == CARD ? CARD_HI[q] : fill[q] + (255 - fill[q]) * 0.2f) : fill[q];
  blend(c, f, a);
  if (ic >= 0) blend(c, icCol, clamp01(0.5f - icon(ic, sx - cx, sy - icY) * k) * al);
  float ta = textA(label, (int)cx, txY, (int)sx, (int)sy);
  if (ta > 0.f) blend(c, txCol, ta * al);
}
// where the tiles and buttons are: tile / button i -> centre and half size
static void itemBox(int i, float& cx, float& cy, float& hw, float& hh) {
  if (!asking) {
    cx = (i & 1 ? 123.f : 6.f) + 55.5f, cy = (i & 2 ? 152.f : 60.f) + 42.f, hw = 55.5f, hh = 42.f;
  } else {
    cx = askOne ? 120.f : i ? 178.5f : 61.5f, cy = 205.f, hw = askOne ? 114.f : 55.5f, hh = 29.f;
  }
}
// tile / button i at a pixel: fully in (al, k = 1) for the page image, or as it is right now
static void itemPixel(int i, float x, float y, float* c, float al, float k, float inv, bool pressed) {
  float cx, cy, hw, hh;
  itemBox(i, cx, cy, hw, hh);
  if (!asking) {
    tilePixel(x, y, c, al, k, inv, pressed, cx, cy, hw, hh, CARD, tiles[i].icon, tiles[i].col, cy - 13.f,
              txt[S_TILE0 + i], CREAM, (int)cy + 8);
  } else {
    bool yes = !askOne && i == 1;
    tilePixel(x, y, c, al, k, inv, pressed, cx, cy, hw, hh, yes ? askCol : CARD, -1, nullptr, 0.f,
              txt[yes || askOne ? S_YES : S_NO], yes ? yesText : CREAM, (int)cy - txt[S_NO].h / 2);
  }
}
// The page below the header as it looks once it has come up, drawn once per page (in its first frame, while
// nothing shows yet, on both cores): the pop-in then only scales and fades this image, a fraction of the work.
EXT_RAM_BSS_ATTR static uint16_t pageImg[240 * 240];  // RGB565, not swapped
static void pageRow(int y) {
  const int gen = pageGen;  // a row started for an older page must not count for a newer one
  for (int x = 0; x < 240; ++x) {
    float c[3] = {(float)NIGHT[0], (float)NIGHT[1], (float)NIGHT[2]};
    const float fx = x + 0.5f, fy = y + 0.5f;
    if (!asking) {
      int i = (x >= 120) + 2 * (y >= 148);
      if (i < nTiles) itemPixel(i, fx, fy, c, 1.f, 1.f, 1.f, false);
    } else if (y < ASK_Y) {  // the question: the icon in a tinted circle, the title, one or two lines
      float d = sqrtf((fx - 120.f) * (fx - 120.f) + (fy - 79.f) * (fy - 79.f)) - 23.f;
      if (d < 1.f) {
        blend(c, chipCol, clamp01(0.5f - d));
        blend(c, askCol, clamp01(0.5f - icon(askIcon, fx - 120.f, fy - 79.f)));
      }
      blend(c, CREAM, textA(txt[S_ASKT], 120, 104, x, y));
      blend(c, MUTED, fmaxf(textA(txt[S_ASKL0], 120, 130, x, y), textA(txt[S_ASKL1], 120, 149, x, y)));
    } else {
      itemPixel(askOne ? 0 : x >= 120, fx, fy, c, 1.f, 1.f, 1.f, false);
    }
    pageImg[y * 240 + x] = (uint16_t)(((int)c[0] & 0xF8) << 8 | ((int)c[1] & 0xFC) << 3 | (int)c[2] >> 3);
  }
  rowGen[y] = gen;
}
// the page image at (x, y) faded in by al over the background bg (integer: this runs for most pixels)
static inline uint16_t fromPage(int x, int y, float al, const float* bg) {
  uint16_t v = pageImg[y * 240 + x];
  if (al >= 1.f) return px((v >> 11) << 3, (v >> 5 & 63) << 2, (v & 31) << 3);
  int a = (int)(al * 256.f), r = (v >> 11) << 3, g = (v >> 5 & 63) << 2, b = (v & 31) << 3;
  int br = (int)bg[0], bgn = (int)bg[1], bb = (int)bg[2];
  return px((uint8_t)(br + ((r - br) * a >> 8)), (uint8_t)(bgn + ((g - bgn) * a >> 8)), (uint8_t)(bb + ((b - bb) * a >> 8)));
}
static void renderMenu(uint16_t* buf, int x0, int y0, int x1, int y1) {
  const float bgk = easeOut(tOpen / 0.15f), ha = easeOut((tOpen - 0.05f) / 0.25f);
  const float bg[3] = {NIGHT[0] * bgk, NIGHT[1] * bgk, NIGHT[2] * bgk};
  for (int y = y0; y < y1; ++y) {
    if (y >= 54 && rowGen[y] != pageGen) pageRow(y);
    for (int x = x0; x < x1; ++x) {
      const float fx = x + 0.5f, fy = y + 0.5f;
      if (y < 54) {  // header, drawn live: the eyes, the status line, close
        float c[3] = {bg[0], bg[1], bg[2]};
        if (y >= 8 && y < 38 && x >= 92 && x < 148) eyesPixel(fx, fy, c, ha);
        blend(c, MUTED, textA(txt[S_STATUS], 120, 39, x, y) * ha);
        if (x < 40 && y >= 6 && y < 40) {  // the microphone, the size of close: its icon scaled down
          float d = sqrtf((fx - 23.f) * (fx - 23.f) + (fy - 23.f) * (fy - 23.f)) - 15.f;
          blend(c, pressedI == HIT_MIC ? CARD_HI : CARD, clamp01(0.5f - d) * ha);
          float di = icon(micOff ? IC_MUTED : IC_MIC, (fx - 23.f) * 1.5f, (fy - 23.f) * 1.5f) / 1.5f;
          blend(c, micOff ? RED : pressedI == HIT_MIC ? CREAM : MUTED, clamp01(0.5f - di) * ha);
        }
        if (x >= 200 && y >= 6 && y < 40) {
          float d = sqrtf((fx - 217.f) * (fx - 217.f) + (fy - 23.f) * (fy - 23.f)) - 15.f;
          blend(c, pressedI == HIT_CLOSE ? CARD_HI : CARD, clamp01(0.5f - d) * ha);
          blend(c, pressedI == HIT_CLOSE ? CREAM : MUTED, clamp01(0.5f - icon(IC_CLOSE, fx - 217.f, fy - 23.f)) * ha);
        }
        *buf++ = px((uint8_t)c[0], (uint8_t)c[1], (uint8_t)c[2]);
        continue;
      }
      int i = -1;  // the tile / button here, -1 the question text
      if (!asking) i = (x >= 120) + 2 * (y >= 148);
      else if (y >= ASK_Y) i = askOne ? 0 : x >= 120;
      if (i >= 0 && i == pressedI) {  // pressed: live, sunk in and lighter
        float c[3] = {bg[0], bg[1], bg[2]};
        itemPixel(i, fx, fy, c, popA[i], popK[i], popInv[i], true);
        *buf++ = px((uint8_t)c[0], (uint8_t)c[1], (uint8_t)c[2]);
        continue;
      }
      float al, inv, cx, cy, hw = 120.f, hh = 120.f;
      if (i >= 0) {
        al = popA[i], inv = popInv[i];
        itemBox(i, cx, cy, hw, hh);
      } else {  // the icon chip pops, the text fades in
        bool chip = (fx - 120.f) * (fx - 120.f) + (fy - 79.f) * (fy - 79.f) < 34.f * 34.f;
        al = chip ? popA[4] : clamp01(popA[4] * 1.2f - 0.3f);
        inv = chip ? popInv[4] : 1.f;
        cx = 120.f, cy = 79.f;
      }
      if (al <= 0.f) {
        *buf++ = px((uint8_t)bg[0], (uint8_t)bg[1], (uint8_t)bg[2]);
        continue;
      }
      // the same pixel of the page image, scaled about the item's centre (nearest neighbour)
      int sx = (int)(cx + (fx - cx) * inv), sy = (int)(cy + (fy - cy) * inv);
      bool in = i < 0 ? sy >= 54 && sy < ASK_Y : fabsf(sx + 0.5f - cx) <= hw + 1.f && fabsf(sy + 0.5f - cy) <= hh + 1.f;
      *buf++ = in && sx >= 0 && sx < 240 ? fromPage(sx, sy, al, bg) : px((uint8_t)bg[0], (uint8_t)bg[1], (uint8_t)bg[2]);
    }
  }
}

// the logo: two eyes looking up to the right, the right one a little bigger (Pixel is never quite symmetric)
static void logoPixel(int x, int y, float* c) {
  for (int i = 0; i < 2; ++i) {
    float s = i ? 1.07f : 1.f, cx = 120.f + (i ? 33.f : -33.f), cy = 84.f - (i ? 2.f : 0.f);
    float px = (x + 0.5f - cx) / s, py = (y + 0.5f - cy) / s;
    float a = clamp01(0.5f - sdPill(px, py, 23.f, 31.f) * s);
    if (a <= 0.f) continue;
    float ix = px - 5.f, iy = py + 5.f;
    float di = sqrtf(ix * ix / (13.f * 13.f) + iy * iy / (19.f * 19.f));
    float ai = clamp01((1.f - di) * 13.f * s);
    float hx = px - 0.f, hy = py + 15.f;
    float ah = clamp01(5.5f - sqrtf(hx * hx + hy * hy)) * ai;
    for (int k = 0; k < 3; ++k) {
      float v = CREAM[k] + (BLUE[k] - CREAM[k]) * ai;
      v += (255.f - v) * ah;
      c[k] += (v - c[k]) * a;
    }
  }
}
// the brand page: the logo's eyes, below the wordmark ("pixel" in blue, "buddy" in lilac) or a title, a line,
// maybe a progress bar (updates)
static const uint8_t LILAC[3] = {168, 123, 255};
static void renderSplash(uint16_t* buf, int x0, int y0, int x1, int y1) {
  const int TITLE_Y = 134, LINE_Y = 176, BAR_Y = 210, BAR_X0 = 50, BAR_X1 = 190, BAR_H = 6;
  int pct = splashPct;
  const TextImg &t1 = txt[S_SPL_T], &t2 = txt[S_SPL_T2];
  const int gap = 7, w = t1.w + (isBrand ? gap + t2.w : 0), m1 = 120 - w / 2 + t1.w / 2, m2 = 120 + w / 2 - t2.w / 2;
  for (int y = y0; y < y1; ++y)
    for (int x = x0; x < x1; ++x) {
      float c[3] = {0, 0, 0};
      if (y < 128) logoPixel(x, y, c);
      if (y >= TITLE_Y && y < TITLE_Y + 40) {
        blend(c, isBrand ? BLUE : CREAM, textA(t1, m1, TITLE_Y, x, y));
        if (isBrand) blend(c, LILAC, textA(t2, m2, TITLE_Y, x, y));
      }
      blend(c, MUTED, textA(txt[S_SPL_L], 120, LINE_Y, x, y));
      if (pct >= 0 && y >= BAR_Y && y < BAR_Y + BAR_H && x >= BAR_X0 && x < BAR_X1) {
        float d = sdPill(x + 0.5f - 120.f, y + 0.5f - (BAR_Y + BAR_H / 2.f), (BAR_X1 - BAR_X0) / 2.f, BAR_H / 2.f);
        float a = clamp01(0.5f - d);
        bool fill = x < BAR_X0 + (BAR_X1 - BAR_X0) * pct / 100;
        for (int k = 0; k < 3; ++k) {
          float v = fill ? BLUE[k] : TRACK[k];  // flat colours, no gradients (docs/brand.md)
          c[k] += (v - c[k]) * a;
        }
      }
      *buf++ = px((uint8_t)c[0], (uint8_t)c[1], (uint8_t)c[2]);
    }
}

void render(uint16_t* buf, int x0, int y0, int x1, int y1) {
  if (isSplash) {
    renderSplash(buf, x0, y0, x1, y1);
    return;
  }
  if (isMenu) {
    renderMenu(buf, x0, y0, x1, y1);
    return;
  }
  int scale = qrN ? QR_BOX / (qrN + 4) : 1;  // 2 modules quiet zone on each side
  int box = scale * (qrN + 4), bx = (240 - box) / 2;
  for (int y = y0; y < y1; ++y)
    for (int x = x0; x < x1; ++x) {
      float w;
      if (qrN && x >= bx && x < bx + box && y >= QR_TOP && y < QR_TOP + box) {
        int mx = (x - bx) / scale - 2, my = (y - QR_TOP) / scale - 2;
        w = !(mx >= 0 && my >= 0 && mx < qrN && my < qrN && qr[my][mx]);
      } else {
        w = fmaxf(fmaxf(textA(txt[S_SHOW0], 120, TITLE_Y, x, y), textA(txt[S_SHOW0 + 1], 120, LINE1_Y, x, y)),
                  fmaxf(textA(txt[S_SHOW0 + 2], 120, LINE2_Y, x, y), textA(txt[S_SHOW0 + 3], 120, LINE3_Y, x, y)));
      }
      uint8_t v = (uint8_t)(w * 255.f);
      *buf++ = px(v, v, v);
    }
}

}  // namespace screen
