// Runtime settings: edited in the web interface, stored as JSON on the SD card
// (/sdcard/pixel/config.json, NVS as fallback), defaults come from config.h.
#pragma once
#include <stdint.h>

#include "eyes.h"

namespace settings {

struct Conv {
  char name[32];
  char persona[4096];
  char voice[24];
  char model[64];          // GPT-Live voice model
  char backendModel[64];   // cheap, fast text model that runs the tools
  char backendEffort[12];  // its reasoning effort
  int volume;              // 0..100
  float pauseS;            // end the session after this much silence
  int resumeS;             // wake word within this time continues the conversation
  bool webSearch;          // let the backend search the web
  char knowledge[6144];    // facts about itself (system prompt)
  char haUrl[128];         // Home Assistant, e.g. http://homeassistant.local:8123
  char haToken[300];       // long-lived access token
  char wake[12];           // "pixel", "halloesp" or "both"
  float wakeSens;          // Pixel sensitivity 0..1 (0.5 = as trained)
  bool wakeBeep;           // short tone when the wake word is heard
  char apiKey[200];        // OpenAI API key (setup page / web interface), empty = the one in secrets.h
  char moodLight[64];      // light.* entity whose colour the irises follow, empty = off
  bool nightOn;            // night mode: dim display, calmer idle, quieter tone
  int nightFrom, nightTo;  // hours 0..23
  int nightBright;         // display brightness at night 0..100
  float budgetDay;         // OpenAI cost limit per day in USD, 0 = none
};

void init();                         // defaults, then load from SD / NVS
uint32_t version();                  // increments on every change
void look(eyes::Look& out);
void conv(Conv& out);
const char* apiKey();                // OpenAI key: from the settings, else secrets.h
int brightness();                    // 0..100
void leds(int& mode, int& bright, uint32_t& color);  // 0 off, 1 status, 2 status + glow; 0..100; 0xRRGGBB, 0 = eyes
int fps();
int displayOffMin();
bool updateAuto();                   // install new versions from GitHub by itself
int updateAt();                      // ... at this minute of the day
int customCount();                   // own animations (sequences of up to 6 steps)
const char* customName(int i);
int custom(const char* name, int* beh); // the steps (behaviour indices) of the one with this name, 0 = none
bool backup(const char* build);      // the settings (without keys) to the SD card
bool backupInfo(long& saved, char* build, int cap);
bool restoreBackup();                 // display off after minutes without anyone (0 = never)
// the time-of-day schedule at a minute of the day: brightness 0..100, volume 0..100 or -1 (as set); false = none
bool scheduled(int minute, int& bright, int& volume);
bool anim(int behavior, bool& on, float& speed, float& intensity);

char* toJson();                      // full settings + metadata, caller frees with free()
bool apply(const char* json);        // merge a (partial) JSON document, save
void reset();                        // back to defaults, save
void factory();                      // like reset, and the OpenAI key goes too

}  // namespace settings
