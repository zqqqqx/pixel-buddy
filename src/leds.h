// The two RGB LEDs (WS2812 chain on GPIO 48).
#pragma once
#include <stdint.h>

namespace leds {

void init();
void set(uint32_t first, uint32_t second);  // 0xRRGGBB each; only sent when something changed

}  // namespace leds
