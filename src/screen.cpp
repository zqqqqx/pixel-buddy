#include "screen.h"

#include <math.h>

#include <initializer_list>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_timer.h"
#include "qrcode.h"

namespace screen {

// classic 5x7 font, ASCII 32..126, 5 columns per glyph, bit 0 = top row
// the hot drawing code runs from internal RAM like the eyes (no flash cache misses while both cores draw)
#define HOT IRAM_ATTR
static const DRAM_ATTR uint8_t FONT[95][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x5F, 0x00, 0x00}, {0x00, 0x07, 0x00, 0x07, 0x00},
    {0x14, 0x7F, 0x14, 0x7F, 0x14}, {0x24, 0x2A, 0x7F, 0x2A, 0x12}, {0x23, 0x13, 0x08, 0x64, 0x62},
    {0x36, 0x49, 0x55, 0x22, 0x50}, {0x00, 0x05, 0x03, 0x00, 0x00}, {0x00, 0x1C, 0x22, 0x41, 0x00},
    {0x00, 0x41, 0x22, 0x1C, 0x00}, {0x14, 0x08, 0x3E, 0x08, 0x14}, {0x08, 0x08, 0x3E, 0x08, 0x08},
    {0x00, 0x50, 0x30, 0x00, 0x00}, {0x08, 0x08, 0x08, 0x08, 0x08}, {0x00, 0x60, 0x60, 0x00, 0x00},
    {0x20, 0x10, 0x08, 0x04, 0x02}, {0x3E, 0x51, 0x49, 0x45, 0x3E}, {0x00, 0x42, 0x7F, 0x40, 0x00},
    {0x42, 0x61, 0x51, 0x49, 0x46}, {0x21, 0x41, 0x45, 0x4B, 0x31}, {0x18, 0x14, 0x12, 0x7F, 0x10},
    {0x27, 0x45, 0x45, 0x45, 0x39}, {0x3C, 0x4A, 0x49, 0x49, 0x30}, {0x01, 0x71, 0x09, 0x05, 0x03},
    {0x36, 0x49, 0x49, 0x49, 0x36}, {0x06, 0x49, 0x49, 0x29, 0x1E}, {0x00, 0x36, 0x36, 0x00, 0x00},
    {0x00, 0x56, 0x36, 0x00, 0x00}, {0x08, 0x14, 0x22, 0x41, 0x00}, {0x14, 0x14, 0x14, 0x14, 0x14},
    {0x00, 0x41, 0x22, 0x14, 0x08}, {0x02, 0x01, 0x51, 0x09, 0x06}, {0x32, 0x49, 0x79, 0x41, 0x3E},
    {0x7E, 0x11, 0x11, 0x11, 0x7E}, {0x7F, 0x49, 0x49, 0x49, 0x36}, {0x3E, 0x41, 0x41, 0x41, 0x22},
    {0x7F, 0x41, 0x41, 0x22, 0x1C}, {0x7F, 0x49, 0x49, 0x49, 0x41}, {0x7F, 0x09, 0x09, 0x09, 0x01},
    {0x3E, 0x41, 0x49, 0x49, 0x7A}, {0x7F, 0x08, 0x08, 0x08, 0x7F}, {0x00, 0x41, 0x7F, 0x41, 0x00},
    {0x20, 0x40, 0x41, 0x3F, 0x01}, {0x7F, 0x08, 0x14, 0x22, 0x41}, {0x7F, 0x40, 0x40, 0x40, 0x40},
    {0x7F, 0x02, 0x0C, 0x02, 0x7F}, {0x7F, 0x04, 0x08, 0x10, 0x7F}, {0x3E, 0x41, 0x41, 0x41, 0x3E},
    {0x7F, 0x09, 0x09, 0x09, 0x06}, {0x3E, 0x41, 0x51, 0x21, 0x5E}, {0x7F, 0x09, 0x19, 0x29, 0x46},
    {0x46, 0x49, 0x49, 0x49, 0x31}, {0x01, 0x01, 0x7F, 0x01, 0x01}, {0x3F, 0x40, 0x40, 0x40, 0x3F},
    {0x1F, 0x20, 0x40, 0x20, 0x1F}, {0x3F, 0x40, 0x38, 0x40, 0x3F}, {0x63, 0x14, 0x08, 0x14, 0x63},
    {0x07, 0x08, 0x70, 0x08, 0x07}, {0x61, 0x51, 0x49, 0x45, 0x43}, {0x00, 0x7F, 0x41, 0x41, 0x00},
    {0x02, 0x04, 0x08, 0x10, 0x20}, {0x00, 0x41, 0x41, 0x7F, 0x00}, {0x04, 0x02, 0x01, 0x02, 0x04},
    {0x40, 0x40, 0x40, 0x40, 0x40}, {0x00, 0x01, 0x02, 0x04, 0x00}, {0x20, 0x54, 0x54, 0x54, 0x78},
    {0x7F, 0x48, 0x44, 0x44, 0x38}, {0x38, 0x44, 0x44, 0x44, 0x20}, {0x38, 0x44, 0x44, 0x48, 0x7F},
    {0x38, 0x54, 0x54, 0x54, 0x18}, {0x08, 0x7E, 0x09, 0x01, 0x02}, {0x0C, 0x52, 0x52, 0x52, 0x3E},
    {0x7F, 0x08, 0x04, 0x04, 0x78}, {0x00, 0x44, 0x7D, 0x40, 0x00}, {0x20, 0x40, 0x44, 0x3D, 0x00},
    {0x7F, 0x10, 0x28, 0x44, 0x00}, {0x00, 0x41, 0x7F, 0x40, 0x00}, {0x7C, 0x04, 0x18, 0x04, 0x78},
    {0x7C, 0x08, 0x04, 0x04, 0x78}, {0x38, 0x44, 0x44, 0x44, 0x38}, {0x7C, 0x14, 0x14, 0x14, 0x08},
    {0x08, 0x14, 0x14, 0x18, 0x7C}, {0x7C, 0x08, 0x04, 0x04, 0x08}, {0x48, 0x54, 0x54, 0x54, 0x20},
    {0x04, 0x3F, 0x44, 0x40, 0x20}, {0x3C, 0x40, 0x40, 0x20, 0x7C}, {0x1C, 0x20, 0x40, 0x20, 0x1C},
    {0x3C, 0x40, 0x30, 0x40, 0x3C}, {0x44, 0x28, 0x10, 0x28, 0x44}, {0x0C, 0x50, 0x50, 0x50, 0x3C},
    {0x44, 0x64, 0x54, 0x4C, 0x44}, {0x00, 0x08, 0x36, 0x41, 0x00}, {0x00, 0x00, 0x7F, 0x00, 0x00},
    {0x00, 0x41, 0x36, 0x08, 0x00}, {0x10, 0x08, 0x08, 0x10, 0x08},
};
// German letters and the middle dot as codes 128..135 (latin() maps UTF-8 to them)
static const DRAM_ATTR uint8_t EXTRA[8][5] = {
    {0x20, 0x55, 0x54, 0x55, 0x78}, {0x38, 0x45, 0x44, 0x45, 0x38}, {0x3C, 0x41, 0x40, 0x21, 0x7C},  // ä ö ü
    {0x79, 0x14, 0x12, 0x14, 0x79}, {0x39, 0x44, 0x44, 0x44, 0x39}, {0x3D, 0x40, 0x40, 0x40, 0x3D},  // Ä Ö Ü
    {0x7E, 0x01, 0x49, 0x56, 0x20}, {0x00, 0x08, 0x1C, 0x08, 0x00},                                  // ß ·
};
static void latin(char* d, int cap, const char* s) {
  static const char* const U[8] = {"ä", "ö", "ü", "Ä", "Ö", "Ü", "ß", "·"};
  int n = 0;
  while (*s && n < cap - 1) {
    int k = 0;
    while (k < 8 && strncmp(s, U[k], 2)) ++k;  // all two bytes in UTF-8
    if (k < 8) {
      d[n++] = (char)(128 + k);
      s += 2;
    } else {
      d[n++] = *s++;
    }
  }
  d[n] = 0;
}

static volatile bool on = false;
static bool isMenu = false, isSplash = false;
static volatile int splashPct = -1;
static volatile uint32_t ver = 0;
static char text[4][40];
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
  latin(text[0], sizeof(text[0]), title);
  latin(text[1], sizeof(text[1]), l1);
  latin(text[2], sizeof(text[2]), l2);
  latin(text[3], sizeof(text[3]), l3);
  esp_qrcode_config_t c = ESP_QRCODE_CONFIG_DEFAULT();
  c.display_func = grab;
  c.max_qrcode_version = 10;
  c.qrcode_ecc_level = ESP_QRCODE_ECC_LOW;
  qrN = 0;
  esp_qrcode_generate(&c, code);
  on = true;
  ver = ver + 1;
}
void splash(const char* title, const char* line, int pct) {
  latin(text[0], sizeof(text[0]), title);
  latin(text[1], sizeof(text[1]), line);
  splashPct = pct;
  qrN = 0;
  isMenu = false;
  isSplash = true;
  on = true;
  ver = ver + 1;
}
void hide() {
  on = false;
  ver = ver + 1;
}
bool active() { return on; }
uint32_t version() { return ver; }

// layout: title (scale 2) at the top, QR code in the middle, two text lines (scale 2) below
// title, QR code, three text lines
static const int TITLE_Y = 6, QR_TOP = 26, QR_BOX = 150, LINE1_Y = 182, LINE2_Y = 202, LINE3_Y = 222;

HOT static bool textPixel(const char* s, int scale, int top, int x, int y, int mid = 120) {
  if (y < top || y >= top + 7 * scale || !s[0]) return false;  // (a shrunk line stays inside this band)
  int n = strlen(s), w = n * 6 * scale - scale;
  if (w > 236 && scale > 1) {  // too long for big letters (e.g. an address): small ones, centred in the line
    top += 7 * (scale - 1) / 2;
    scale = 1;
    w = n * 6 - 1;
  }
  int left = mid - w / 2;
  if (y < top || y >= top + 7 * scale || x < left || x >= left + w) return false;
  int cx = (x - left) / scale, cy = (y - top) / scale;
  int ch = cx / 6, col = cx % 6;
  if (col == 5) return false;
  unsigned char c = (unsigned char)s[ch];
  if (c >= 128 && c < 136) return EXTRA[c - 128][col] >> cy & 1;
  if (c < 32 || c > 126) c = '?';
  return FONT[c - 32][col] >> cy & 1;
}

static inline uint16_t px(uint8_t r, uint8_t g, uint8_t b) {
  uint16_t v = (uint16_t)((r & 0xF8) << 8 | (g & 0xFC) << 3 | b >> 3);
#ifdef EYES_SWAP_BYTES
  v = (uint16_t)(v >> 8 | v << 8);
#endif
  return v;
}

// brand colours (docs/brand.md)
static const uint8_t CREAM[3] = {246, 243, 234}, BLUE[3] = {61, 107, 255}, MUTED[3] = {139, 148, 169},
                     TRACK[3] = {37, 43, 58};
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
// ------------------------------------------------------------------ the menu (BOOT 5x)
// Up to four big tiles (2 x 2, each a whole quarter of the screen to hit), or a question with No / Yes:
// nothing happens on a single stray touch. Small Pixel Buddy eyes on top watch the finger and blink.
// Pages pop in (tiles one after the other, with a little overshoot); a pressed tile sinks in and acts
// when the finger lets go on it. Flat brand colours (docs/brand.md).
static const uint8_t NIGHT[3] = {11, 13, 18}, CARD[3] = {24, 28, 40}, CARD_HI[3] = {40, 47, 66};
struct MTile {
  char label[16];
  int icon;
  uint8_t col[3];
};
static MTile tiles[4];
static int nTiles = 0;
static bool asking = false, askOne = false;  // a question; with a single OK button
static int askIcon = 0, eyeMood = 0;         // eyes: 0 curious, 1 worried, 2 sleepy
static uint8_t askCol[3], chipCol[3], yesText[3];
static char askTitle[24], askLine[2][44], askYes[16], status[56];
static volatile int pressedI = -1;
static int64_t openT0 = 0, pageT0 = 0, blinkT0 = 0, nextBlink = 0, lookUntil = 0;
static float tPage = 9.f, tOpen = 9.f, blinkK = 1.f;  // taken once per frame: all bands alike
static float lookX = 0.f, lookY = 0.f, lookTX = 0.f, lookTY = 0.f;
static float popA[5], popK[5], popInv[5];  // per tile / button this frame: alpha and scale (the pop-in, pressed); 4 the text
static float drawnA[5], drawnK[5], drawnHa = -1.f, drawnBg = -1.f;  // as last drawn: unchanged ones are skipped
static int drawnPressed = -2;
static volatile int pageGen = 1;  // the page image below (pageRow) is redrawn when this changes
static int rowGen[240];
static volatile int rowsBuilt = 0;  // of this page image (rows 54..239, both cores add)
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
  rowsBuilt = 0;
  drawnBg = -1.f;  // all of it once
  pressedI = -1;
  qrN = 0;
  isSplash = false;
  isMenu = true;
  on = true;
  ver = ver + 1;
}
void menu(const char* st, const char* const* labels, const int* icons, const uint32_t* colors, int n) {
  latin(status, sizeof(status), st);
  nTiles = n < 4 ? n : 4;
  for (int i = 0; i < nTiles; ++i) {
    latin(tiles[i].label, sizeof(tiles[i].label), labels[i]);
    tiles[i].icon = icons[i];
    rgb(colors[i], tiles[i].col);
  }
  asking = false;
  eyeMood = 0;
  page();
}
void ask(const char* st, int icon, uint32_t color, const char* title, const char* line, const char* yes, int mood) {
  latin(status, sizeof(status), st);
  askIcon = icon;
  rgb(color, askCol);
  for (int k = 0; k < 3; ++k) chipCol[k] = (uint8_t)(NIGHT[k] + (askCol[k] - NIGHT[k]) * 0.2f);  // flat tint
  bool light = askCol[0] * 0.3f + askCol[1] * 0.59f + askCol[2] * 0.11f > 140.f;
  memcpy(yesText, light ? NIGHT : CREAM, 3);
  latin(askTitle, sizeof(askTitle), title);
  const char* nl = strchr(line, '\n');  // up to two lines
  char l0[44];
  snprintf(l0, sizeof(l0), "%.*s", nl ? (int)(nl - line) : (int)strlen(line), line);
  latin(askLine[0], sizeof(askLine[0]), l0);
  latin(askLine[1], sizeof(askLine[1]), nl ? nl + 1 : "");
  askOne = !yes;
  latin(askYes, sizeof(askYes), yes ? yes : "OK");
  eyeMood = mood;
  asking = true;
  page();
}
int hit(int x, int y) {
  if (!on || !isMenu) return -1;
  if (y < 54) return x >= 190 ? HIT_CLOSE : x >= 86 && x < 156 ? HIT_EYES : -1;
  if (!asking) {
    int i = (x >= 120) + 2 * (y >= 148);
    return i < nTiles ? i : -1;
  }
  if (y < 164) return -1;
  return askOne ? 0 : x >= 120;
}
void press(int i) { pressedI = i; }
void lookAt(int x, int y) {
  lookTX = fmaxf(-1.f, fminf(1.f, (x - 120) / 90.f));
  lookTY = fmaxf(-1.f, fminf(1.f, (y - 23) / 110.f));
  lookUntil = esp_timer_get_time() + 1500000;
}
void poke() { nextBlink = 0; }
bool animating(int& x0, int& y0, int& x1, int& y1) {
  if (!on || !isMenu) return false;
  int64_t now = esp_timer_get_time();
  if (rowsBuilt < 240 - 54) pageT0 = now;  // the pop-in starts once the page image is complete
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
  if (ha != drawnHa || pressedI == HIT_CLOSE || drawnPressed == HIT_CLOSE) add(0, 0, 240, 54);
  for (int i = 0; i < 5; ++i) {
    bool moved = popA[i] != drawnA[i] || popK[i] != drawnK[i] || (pressedI != drawnPressed && (i == pressedI || i == drawnPressed));
    if (!moved) continue;
    if (i == 4) add(0, 54, 240, asking ? 164 : 54);
    else if (!asking) add(i & 1 ? 120 : 0, i & 2 ? 148 : 54, i & 1 ? 240 : 120, i & 2 ? 240 : 148);
    else add(askOne || i == 0 ? 0 : 120, 164, askOne || i == 1 ? 240 : 120, 240);
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
HOT static float sdArc(float x, float y, float R, float gx, float gy, float g) {
  float u = -(x * gx + y * gy), v = x * gy - y * gx;  // the gap at angle pi
  if (fabsf(atan2f(v, u)) <= (float)M_PI - g) return fabsf(sqrtf(x * x + y * y) - R);
  float ex = -R * cosf(g), ey = R * sinf(g);
  return fminf(sqrtf((u - ex) * (u - ex) + (v - ey) * (v - ey)), sqrtf((u - ex) * (u - ex) + (v + ey) * (v + ey)));
}
// line icons, 2.8 px strokes, about 26 px tall, centred on (0, 0): signed distance
HOT static float icon(int k, float x, float y) {
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
  }
  return d - 1.4f;
}
// the small Pixel Buddy eyes in the header (like the logo): look around, blink, worry, get sleepy
HOT static void eyesPixel(float x, float y, float* c, float al) {
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
HOT static void tilePixel(float x, float y, float* c, float al, float k, float inv, bool pressed, float cx, float cy,
                          float hw, float hh, const uint8_t* fill, int ic, const uint8_t* icCol, float icY,
                          const char* label, const uint8_t* txCol, int txY) {
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
  if (textPixel(label, 2, txY, (int)sx, (int)sy, (int)cx)) blend(c, txCol, al);
}
// where the tiles and buttons are: tile / button i -> centre and half size
static void itemBox(int i, float& cx, float& cy, float& hw, float& hh) {
  if (!asking) {
    cx = (i & 1 ? 123.f : 6.f) + 55.5f, cy = (i & 2 ? 152.f : 60.f) + 42.f, hw = 55.5f, hh = 42.f;
  } else {
    cx = askOne ? 120.f : i ? 178.5f : 61.5f, cy = 202.f, hw = askOne ? 114.f : 55.5f, hh = 32.f;
  }
}
// tile / button i at a pixel: fully in (al, k = 1) for the page image, or as it is right now
HOT static void itemPixel(int i, float x, float y, float* c, float al, float k, float inv, bool pressed) {
  float cx, cy, hw, hh;
  itemBox(i, cx, cy, hw, hh);
  if (!asking) {
    tilePixel(x, y, c, al, k, inv, pressed, cx, cy, hw, hh, CARD, tiles[i].icon, tiles[i].col, cy - 11.f, tiles[i].label,
              CREAM, (int)cy + 15);
  } else {
    bool yes = !askOne && i == 1;
    tilePixel(x, y, c, al, k, inv, pressed, cx, cy, hw, hh, yes ? askCol : CARD, -1, nullptr, 0.f,
              yes || askOne ? askYes : "Nein", yes ? yesText : CREAM, 195);
  }
}
// The page below the header as it looks once it has come up, drawn once per page (in its first frame, while
// nothing shows yet, on both cores): the pop-in then only scales and fades this image, a fraction of the work.
EXT_RAM_BSS_ATTR static uint16_t pageImg[240 * 240];  // RGB565, not swapped
HOT static void pageRow(int y) {
  for (int x = 0; x < 240; ++x) {
    float c[3] = {(float)NIGHT[0], (float)NIGHT[1], (float)NIGHT[2]};
    const float fx = x + 0.5f, fy = y + 0.5f;
    if (!asking) {
      int i = (x >= 120) + 2 * (y >= 148);
      if (i < nTiles) itemPixel(i, fx, fy, c, 1.f, 1.f, 1.f, false);
    } else if (y < 164) {  // the question: the icon in a tinted circle, the title, one or two lines
      float d = sqrtf((fx - 120.f) * (fx - 120.f) + (fy - 79.f) * (fy - 79.f)) - 23.f;
      if (d < 1.f) {
        blend(c, chipCol, clamp01(0.5f - d));
        blend(c, askCol, clamp01(0.5f - icon(askIcon, fx - 120.f, fy - 79.f)));
      }
      if (textPixel(askTitle, 2, 109, x, y)) blend(c, CREAM, 1.f);
      if (textPixel(askLine[0], 2, 130, x, y) || textPixel(askLine[1], 2, 147, x, y)) blend(c, MUTED, 1.f);
    } else {
      itemPixel(askOne ? 0 : x >= 120, fx, fy, c, 1.f, 1.f, 1.f, false);
    }
    pageImg[y * 240 + x] = (uint16_t)(((int)c[0] & 0xF8) << 8 | ((int)c[1] & 0xFC) << 3 | (int)c[2] >> 3);
  }
  rowGen[y] = pageGen;
  __atomic_fetch_add(&rowsBuilt, 1, __ATOMIC_RELAXED);
}
// the page image at (x, y) faded in by al over the background bg (integer: this runs for most pixels)
HOT static inline uint16_t fromPage(int x, int y, float al, const float* bg) {
  uint16_t v = pageImg[y * 240 + x];
  if (al >= 1.f) return px((v >> 11) << 3, (v >> 5 & 63) << 2, (v & 31) << 3);
  int a = (int)(al * 256.f), r = (v >> 11) << 3, g = (v >> 5 & 63) << 2, b = (v & 31) << 3;
  int br = (int)bg[0], bgn = (int)bg[1], bb = (int)bg[2];
  return px((uint8_t)(br + ((r - br) * a >> 8)), (uint8_t)(bgn + ((g - bgn) * a >> 8)), (uint8_t)(bb + ((b - bb) * a >> 8)));
}
HOT static void renderMenu(uint16_t* buf, int x0, int y0, int x1, int y1) {
  const float bgk = easeOut(tOpen / 0.15f), ha = easeOut((tOpen - 0.05f) / 0.25f);
  const float bg[3] = {NIGHT[0] * bgk, NIGHT[1] * bgk, NIGHT[2] * bgk};
  for (int y = y0; y < y1; ++y) {
    if (y >= 54 && rowGen[y] != pageGen) pageRow(y);
    for (int x = x0; x < x1; ++x) {
      const float fx = x + 0.5f, fy = y + 0.5f;
      if (y < 54) {  // header, drawn live: the eyes, the status line, close
        float c[3] = {bg[0], bg[1], bg[2]};
        if (y >= 8 && y < 38 && x >= 92 && x < 148) eyesPixel(fx, fy, c, ha);
        if (textPixel(status, 1, 44, x, y)) blend(c, MUTED, ha);
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
      else if (y >= 164) i = askOne ? 0 : x >= 120;
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
      bool in = i < 0 ? sy >= 54 && sy < 164 : fabsf(sx + 0.5f - cx) <= hw + 1.f && fabsf(sy + 0.5f - cy) <= hh + 1.f;
      *buf++ = in && sx >= 0 && sx < 240 ? fromPage(sx, sy, al, bg) : px((uint8_t)bg[0], (uint8_t)bg[1], (uint8_t)bg[2]);
    }
  }
}

// the logo: two eyes looking up to the right, the right one a little bigger (Pixel is never quite symmetric)
HOT static void logoPixel(int x, int y, float* c) {
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
HOT static void renderSplash(uint16_t* buf, int x0, int y0, int x1, int y1) {
  const int TITLE_Y = 142, LINE_Y = 176, BAR_Y = 204, BAR_X0 = 40, BAR_X1 = 200, BAR_H = 8;
  int pct = splashPct;
  for (int y = y0; y < y1; ++y)
    for (int x = x0; x < x1; ++x) {
      float c[3] = {0, 0, 0};
      if (y < 130) logoPixel(x, y, c);
      if (textPixel(text[0], 3, TITLE_Y, x, y)) c[0] = CREAM[0], c[1] = CREAM[1], c[2] = CREAM[2];
      if (textPixel(text[1], pct >= 0 ? 1 : 2, pct >= 0 ? 226 : LINE_Y, x, y))
        c[0] = MUTED[0], c[1] = MUTED[1], c[2] = MUTED[2];
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

HOT void render(uint16_t* buf, int x0, int y0, int x1, int y1) {
  if (isSplash) {
    renderSplash(buf, x0, y0, x1, y1);
    return;
  }
  if (isMenu) {
    renderMenu(buf, x0, y0, x1, y1);
    return;
  }
  const uint16_t BLACK = 0, WHITE = 0xFFFF;
  int scale = qrN ? QR_BOX / (qrN + 4) : 1;  // 2 modules quiet zone on each side
  int box = scale * (qrN + 4), bx = (240 - box) / 2;
  for (int y = y0; y < y1; ++y)
    for (int x = x0; x < x1; ++x) {
      bool white;
      if (qrN && x >= bx && x < bx + box && y >= QR_TOP && y < QR_TOP + box) {
        int mx = (x - bx) / scale - 2, my = (y - QR_TOP) / scale - 2;
        white = !(mx >= 0 && my >= 0 && mx < qrN && my < qrN && qr[my][mx]);
      } else {
        white = textPixel(text[0], 2, TITLE_Y, x, y) || textPixel(text[1], 2, LINE1_Y, x, y) ||
                textPixel(text[2], 2, LINE2_Y, x, y) || textPixel(text[3], 2, LINE3_Y, x, y);
      }
      *buf++ = white ? WHITE : BLACK;  // 0xFFFF / 0 are the same with swapped bytes
    }
}

}  // namespace screen
