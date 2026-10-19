// Full screen pages instead of the eyes: setup (title, QR code, lines), the menu, the brand splash.
#pragma once
#include <stdint.h>

namespace screen {

void show(const char* title, const char* qr, const char* line1, const char* line2, const char* line3 = "");
// The BOOT 5x menu: up to 4 big tiles (labels, IC_ icons, 0xRRGGBB colours), or a question with No / Yes
// (yes = nullptr: just one button saying no; eyes 0 curious, 1 worried, 2 sleepy). Text: UTF-8, the brand font.
enum { IC_RESTART, IC_HOTSPOT, IC_HOME, IC_POWER, IC_RESET, IC_CLOSE };
const int HIT_CLOSE = 10, HIT_EYES = 11;
void menu(const char* status, const char* const* labels, const int* icons, const uint32_t* colors, int n);
void ask(const char* status, int icon, uint32_t color, const char* title, const char* line, const char* yes,
         const char* no, int mood);
int hit(int x, int y);  // tile / button under a touch (No 0, Yes 1), HIT_CLOSE, HIT_EYES or -1
void press(int i);      // drawn pressed (-1 none)
void lookAt(int x, int y);  // the eyes look at the finger
void poke();                // the eyes blink
// the menu is open: what to redraw this frame (all while a page comes up, else the eyes and what was
// pressed); call once per frame
bool animating(int& x0, int& y0, int& x1, int& y1);
// the brand page: the Pixel Buddy eyes, a title, a line below; pct >= 0 adds a progress bar (updates)
void splash(const char* title, const char* line, int pct = -1);
void brand(const char* line);  // the boot page: the eyes, the "pixel buddy" wordmark, a line (the version)
void hide();
bool active();
uint32_t version();  // changes whenever the page changes (redraw)
void render(uint16_t* buf, int x0, int y0, int x1, int y1);  // RGB565 like eyes::render

}  // namespace screen
