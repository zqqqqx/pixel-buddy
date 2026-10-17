// Home Assistant REST API (smart home control for the voice assistant).
#pragma once

#include "cJSON.h"
#include "settings.h"

namespace ha {

bool configured(const settings::Conv& c);
// "entity_id|name|state" per line for controllable devices and climate sensors; returns line count or -1
int listDevices(const settings::Conv& c, char* out, int cap);
// action: turn_on, turn_off, toggle, set_brightness, set_temperature, set_position, open, close, get_state
bool control(const settings::Conv& c, const char* entity, const char* action, double value, bool hasValue,
             char* result, int cap);
// set_light tool (entity_id, on, rgb_color, brightness_pct, brightness_step_pct, color_temp_kelvin, effect).
// rgb: the colour the lamp has now (0xRRGGBB, for the eyes); anim: light_on, light_off or light_color
bool setLight(const settings::Conv& c, const cJSON* args, char* result, int cap, uint32_t& rgb, const char*& anim);
// condition of the first weather entity (sunny, rainy, ...)
bool weather(const settings::Conv& c, char* cond, int cap);

// lamps changed anywhere (WebSocket event stream); change: 0 none to show, 1 on, 2 off, 3 colour
struct LightEvent {
  char entity[48];
  bool on;
  uint32_t rgb;
  int change;
};
void startEvents();
bool popLight(LightEvent& e);

}  // namespace ha
