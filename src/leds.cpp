#include "leds.h"

#include "esp_log.h"
#include "led_strip.h"

namespace leds {

static led_strip_handle_t strip = nullptr;
static uint32_t shown[2] = {1, 1};  // forces the first write

void init() {
  led_strip_config_t sc = {};
  sc.strip_gpio_num = 48;
  sc.max_leds = 2;
  sc.led_model = LED_MODEL_WS2812;
  sc.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;
  led_strip_rmt_config_t rc = {};
  rc.resolution_hz = 10 * 1000 * 1000;
  if (led_strip_new_rmt_device(&sc, &rc, &strip) != ESP_OK) {
    ESP_LOGW("leds", "no LED driver");
    strip = nullptr;
    return;
  }
  set(0, 0);
}

void set(uint32_t a, uint32_t b) {
  if (!strip || (a == shown[0] && b == shown[1])) return;
  shown[0] = a;
  shown[1] = b;
  led_strip_set_pixel(strip, 0, a >> 16 & 255, a >> 8 & 255, a & 255);
  led_strip_set_pixel(strip, 1, b >> 16 & 255, b >> 8 & 255, b & 255);
  led_strip_refresh(strip);
}

}  // namespace leds
