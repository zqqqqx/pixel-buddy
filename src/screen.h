// Full screen info page instead of the eyes (setup): a title, a QR code and up to two lines of text.
#pragma once
#include <stdint.h>

namespace screen {

void show(const char* title, const char* qr, const char* line1, const char* line2, const char* line3 = "");
// buttons to tap (up to 5), flying in; danger: index drawn in red, -1 none; the last one is the way back
void menu(const char* title, const char* const* items, int n, int danger);
bool animating();       // the menu is still flying in: redraw the whole screen this frame (call once per frame)
int hit(int x, int y);  // menu button under a tap, -1 = none
// the brand page: the Pixel Buddy eyes, a title, a line below; pct >= 0 adds a progress bar (updates)
void splash(const char* title, const char* line, int pct = -1);
void hide();
bool active();
uint32_t version();  // changes whenever the page changes (redraw)
void render(uint16_t* buf, int x0, int y0, int x1, int y1);  // RGB565 like eyes::render

}  // namespace screen
