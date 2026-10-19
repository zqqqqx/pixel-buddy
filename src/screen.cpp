#include "screen.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_timer.h"
#include "qrcode.h"

namespace screen {

// classic 5x7 font, ASCII 32..126, 5 columns per glyph, bit 0 = top row
static const uint8_t FONT[95][5] = {
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

static volatile bool on = false;
static bool isMenu = false, isSplash = false;
static volatile int splashPct = -1;
static char items[5][24];
static int nItems = 0, dangerItem = -1;
static const int BTN_X0 = 12, BTN_X1 = 228, BTN_Y0 = 44, BTN_H = 33, BTN_GAP = 6;  // up to 5 buttons
static volatile uint32_t ver = 0;
static int64_t menuT0 = 0;          // when the menu appeared: it flies in
static volatile float animT = 9.f;  // seconds since then, taken once per frame (all bands alike)
static const float MENU_IN = 0.5f;
static char text[4][40];
EXT_RAM_BSS_ATTR static uint8_t qr[64][64];  // modules (version <= 10 -> 57 x 57)
static int qrN = 0;

static void grab(esp_qrcode_handle_t q) {
  qrN = esp_qrcode_get_size(q);
  if (qrN > 64) qrN = 0;
  for (int y = 0; y < qrN; ++y)
    for (int x = 0; x < qrN; ++x) qr[y][x] = esp_qrcode_get_module(q, x, y);
}

void menu(const char* title, const char* const* it, int n, int danger) {
  snprintf(text[0], sizeof(text[0]), "%s", title);
  text[1][0] = text[2][0] = text[3][0] = 0;
  nItems = n < 5 ? n : 5;
  for (int i = 0; i < nItems; ++i) snprintf(items[i], sizeof(items[0]), "%s", it[i]);
  dangerItem = danger;
  qrN = 0;
  menuT0 = esp_timer_get_time();
  animT = 0.f;
  isSplash = false;
  isMenu = true;
  on = true;
  ver = ver + 1;
}
int hit(int x, int y) {
  if (!on || !isMenu || x < BTN_X0 || x >= BTN_X1) return -1;
  for (int i = 0; i < nItems; ++i) {
    int y0 = BTN_Y0 + i * (BTN_H + BTN_GAP);
    if (y >= y0 - 4 && y < y0 + BTN_H + 4) return i;  // a little slack for fingers
  }
  return -1;
}

void show(const char* title, const char* code, const char* l1, const char* l2, const char* l3) {
  isMenu = isSplash = false;
  snprintf(text[0], sizeof(text[0]), "%s", title);
  snprintf(text[1], sizeof(text[1]), "%s", l1);
  snprintf(text[2], sizeof(text[2]), "%s", l2);
  snprintf(text[3], sizeof(text[3]), "%s", l3);
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
  snprintf(text[0], sizeof(text[0]), "%s", title);
  snprintf(text[1], sizeof(text[1]), "%s", line);
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
bool animating() {
  if (!on || !isMenu) return false;
  float t = (esp_timer_get_time() - menuT0) * 1e-6f;
  animT = t < MENU_IN ? t : MENU_IN;
  return t < MENU_IN + 0.05f;  // one more frame at the end position
}
uint32_t version() { return ver; }

// layout: title (scale 2) at the top, QR code in the middle, two text lines (scale 2) below
// title, QR code, three text lines
static const int TITLE_Y = 6, QR_TOP = 26, QR_BOX = 150, LINE1_Y = 182, LINE2_Y = 202, LINE3_Y = 222;

static bool textPixel(const char* s, int scale, int top, int x, int y) {
  int n = strlen(s), w = n * 6 * scale - scale;
  if (w > 236 && scale > 1) {  // too long for big letters (e.g. an address): small ones, centred in the line
    top += 7 * (scale - 1) / 2;
    scale = 1;
    w = n * 6 - 1;
  }
  int left = (240 - w) / 2;
  if (y < top || y >= top + 7 * scale || x < left || x >= left + w) return false;
  int cx = (x - left) / scale, cy = (y - top) / scale;
  int ch = cx / 6, col = cx % 6;
  if (col == 5) return false;
  unsigned char c = (unsigned char)s[ch];
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
// The menu: brand night background, title with a short blue line, the buttons as pills. They fly in one
// after the other (rise and fade, ease out). The last button is the way back: outlined only.
static void renderMenu(uint16_t* buf, int x0, int y0, int x1, int y1) {
  static const uint8_t NIGHT[3] = {11, 13, 18}, CARD[3] = {27, 32, 46}, ROUGE[3] = {255, 104, 156},
                       ROUGE_BG[3] = {64, 24, 42}, LINE[3] = {60, 68, 90};
  const float t = animT, hw = (BTN_X1 - BTN_X0) / 2.f, hh = BTN_H / 2.f;
  const float bg = easeOut(t / 0.18f), ta = easeOut(t / 0.25f);
  const int titleY = 12 - (int)lroundf((1.f - ta) * 8.f);
  float ba[5];
  int by[5];
  for (int i = 0; i < nItems; ++i) {
    float e = easeOut((t - 0.08f - i * 0.05f) / 0.26f);
    ba[i] = e;
    by[i] = BTN_Y0 + i * (BTN_H + BTN_GAP) + (int)lroundf((1.f - e) * 16.f);
  }
  for (int y = y0; y < y1; ++y)
    for (int x = x0; x < x1; ++x) {
      float c[3] = {NIGHT[0] * bg, NIGHT[1] * bg, NIGHT[2] * bg};
      if (textPixel(text[0], 2, titleY, x, y)) blend(c, CREAM, ta);
      if (y >= titleY + 19 && y < titleY + 22 && x >= 108 && x < 132) blend(c, BLUE, ta);
      for (int i = 0; i < nItems; ++i) {
        if (ba[i] <= 0.f || y < by[i] - 1 || y > by[i] + BTN_H) continue;
        float d = sdPill(x + 0.5f - 120.f, y + 0.5f - (by[i] + hh), hw, hh);
        bool back = i == nItems - 1 && i != dangerItem;
        const uint8_t* fg = i == dangerItem ? ROUGE : back ? MUTED : CREAM;
        float a = back ? clamp01(1.f - fabsf(d + 0.75f)) : clamp01(0.5f - d);  // outline 1.5 px / filled
        if (a > 0.f) blend(c, back ? LINE : i == dangerItem ? ROUGE_BG : CARD, a * ba[i]);
        if (textPixel(items[i], 2, by[i] + (BTN_H - 14) / 2, x, y)) blend(c, fg, ba[i]);
      }
      *buf++ = px((uint8_t)c[0], (uint8_t)c[1], (uint8_t)c[2]);
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
static void renderSplash(uint16_t* buf, int x0, int y0, int x1, int y1) {
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

void render(uint16_t* buf, int x0, int y0, int x1, int y1) {
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
