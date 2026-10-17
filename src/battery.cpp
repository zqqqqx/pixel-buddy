#include "battery.h"

#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"

namespace battery {

static const char* TAG = "battery";
static adc_oneshot_unit_handle_t adc = nullptr;
static float avg = -1;
static int lvl = -1;

void init() {
  gpio_config_t g = {};
  g.pin_bit_mask = 1ULL << GPIO_NUM_41;
  g.mode = GPIO_MODE_INPUT;
  g.pull_up_en = GPIO_PULLUP_ENABLE;
  gpio_config(&g);
  adc_oneshot_unit_init_cfg_t u = {};
  u.unit_id = ADC_UNIT_1;
  adc_oneshot_chan_cfg_t c = {};
  c.atten = ADC_ATTEN_DB_12;
  c.bitwidth = ADC_BITWIDTH_12;
  if (adc_oneshot_new_unit(&u, &adc) != ESP_OK || adc_oneshot_config_channel(adc, ADC_CHANNEL_0, &c) != ESP_OK) {
    ESP_LOGW(TAG, "ADC not available");
    adc = nullptr;
  }
  tick();
}

void tick() {
  int raw;
  if (!adc || adc_oneshot_read(adc, ADC_CHANNEL_0, &raw) != ESP_OK) return;
  avg = avg < 0 ? raw : avg + (raw - avg) * 0.3f;
  // raw readings of the stock firmware: 1980 empty .. 2480 full (12 dB, 12 bit); far below: no battery
  static const int A[] = {1980, 2081, 2163, 2250, 2340, 2480}, L[] = {0, 20, 40, 60, 80, 100};
  if (avg < 1500) {
    lvl = -1;
  } else if (avg <= A[0]) {
    lvl = 0;
  } else if (avg >= A[5]) {
    lvl = 100;
  } else {
    for (int i = 0; i < 5; ++i)
      if (avg < A[i + 1]) {
        lvl = L[i] + (int)((avg - A[i]) * (L[i + 1] - L[i]) / (A[i + 1] - A[i]));
        break;
      }
  }
  static int logged = -2;
  if (lvl / 5 != logged / 5) ESP_LOGI(TAG, "%d %% (raw %.0f)%s", lvl, avg, charging() ? ", charging" : "");
  logged = lvl;
}

int level() { return lvl; }
bool charging() { return gpio_get_level(GPIO_NUM_41) == 1 && lvl < 100; }

}  // namespace battery
