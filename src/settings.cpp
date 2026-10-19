#include "settings.h"
#include "presets.h"
#include "i18n.h"
#include "mic.h"
#include "sched.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include <initializer_list>

#include "cJSON.h"
#include "config.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "creds.h"
#include "sd.h"

namespace settings {

static const char* TAG = "settings";
static const char* PATH = "/sdcard/pixel/config.json";

// Everything about itself is built into the firmware (live.cpp ABOUT): this is for what the owner adds
static const char* DEFAULT_KNOWLEDGE = "";

struct All {
  Conv c;
  eyes::Look l;
  int brightness, fps;
  int ledMode, ledBright;
  uint32_t ledColor;              // 0 = the colour of the eyes
  int offMin;                     // display off after this many minutes without anyone (0 = never)
  bool upAuto;                    // updates from GitHub by itself ...
  int upAt;                       // ... at this minute of the day
  bool schedOn;                   // brightness by time of day
  int schedN, schedAt[8], schedB[8], schedV[8];  // from minute schedAt (of the day): brightness, volume (-1 = as set)
  int cuN, cuLen[8];              // own animations: a name and up to 6 steps (animation names)
  char cuName[8][24], cuStep[8][6][16];
  bool on[64];
  float speed[64], intensity[64];
};
EXT_RAM_BSS_ATTR static All S;
static SemaphoreHandle_t mtx;
static volatile uint32_t ver = 1;

static void defaults() {
  memset(&S, 0, sizeof(S));
  snprintf(S.c.name, sizeof(S.c.name), "%s", CFG_ROBOT_NAME);
  snprintf(S.c.persona, sizeof(S.c.persona), "%s", i18n::LANGS[0].persona);
  snprintf(S.c.voice, sizeof(S.c.voice), "%s", CFG_LIVE_VOICE);
  snprintf(S.c.model, sizeof(S.c.model), "%s", CFG_LIVE_MODEL);
  snprintf(S.c.backendModel, sizeof(S.c.backendModel), "%s", CFG_BACKEND_MODEL);
  snprintf(S.c.backendEffort, sizeof(S.c.backendEffort), "%s", CFG_BACKEND_EFFORT);
  S.c.volume = CFG_SPEAKER_VOLUME;
  S.c.pauseS = 4.f;
  S.c.resumeS = 120;
  S.c.webSearch = true;
  snprintf(S.c.wake, sizeof(S.c.wake), "pixel");
  snprintf(S.c.lang, sizeof(S.c.lang), "en");
  S.c.wakeSens = 0.5f;
  snprintf(S.c.knowledge, sizeof(S.c.knowledge), "%s", DEFAULT_KNOWLEDGE);
  S.c.wakeBeep = true;
  S.c.nightFrom = 22;
  S.c.nightTo = 7;
  S.c.nightBright = 25;
  S.l = eyes::Look();
  S.l.colorway = CFG_COLORWAY;
  S.l.highlights = CFG_HIGHLIGHTS;
  S.l.sleepAfter = CFG_SLEEP_AFTER_S;
  S.l.idleMin = CFG_IDLE_MIN_S;
  S.l.idleMax = CFG_IDLE_MAX_S;
  S.brightness = (int)(CFG_BRIGHTNESS * 100);
  S.fps = CFG_MAX_FPS;
  S.ledMode = 1;
  S.ledBright = 40;
  S.ledColor = 0;
  S.upAuto = true;
  S.upAt = 3 * 60 + 30;  // at night, when nobody talks to it
  S.schedN = 3;  // an example, off until switched on
  S.schedAt[0] = 7 * 60, S.schedB[0] = 100, S.schedV[0] = -1;
  S.schedAt[1] = 19 * 60, S.schedB[1] = 60, S.schedV[1] = -1;
  S.schedAt[2] = 22 * 60, S.schedB[2] = 20, S.schedV[2] = 40;
  for (int i = 0; i < 64; ++i) {
    S.on[i] = true;
    S.speed[i] = 1.f;
    S.intensity[i] = 1.f;
  }
}

// ---------------------------------------------------------------- JSON helpers
static void colorOut(cJSON* o, const char* k, uint32_t c) {
  char b[8];
  snprintf(b, sizeof(b), "#%06lX", (unsigned long)(c & 0xFFFFFF));
  cJSON_AddStringToObject(o, k, b);
}
static void colorIn(const cJSON* o, const char* k, uint32_t& c) {
  const cJSON* v = cJSON_GetObjectItem(o, k);
  if (cJSON_IsString(v) && v->valuestring[0] == '#') c = strtoul(v->valuestring + 1, nullptr, 16);
}
static void num(const cJSON* o, const char* k, float& f) {
  const cJSON* v = cJSON_GetObjectItem(o, k);
  if (cJSON_IsNumber(v)) f = (float)v->valuedouble;
}
static void num(const cJSON* o, const char* k, int& i) {
  const cJSON* v = cJSON_GetObjectItem(o, k);
  if (cJSON_IsNumber(v)) i = v->valueint;
}
static void flag(const cJSON* o, const char* k, bool& b) {
  const cJSON* v = cJSON_GetObjectItem(o, k);
  if (cJSON_IsBool(v)) b = cJSON_IsTrue(v);
}
static void str(const cJSON* o, const char* k, char* dst, size_t n) {
  const cJSON* v = cJSON_GetObjectItem(o, k);
  if (cJSON_IsString(v)) snprintf(dst, n, "%s", v->valuestring);
}

static cJSON* build(bool withMeta) {
  cJSON* r = cJSON_CreateObject();
  cJSON* c = cJSON_AddObjectToObject(r, "conversation");
  cJSON_AddStringToObject(c, "name", S.c.name);
  cJSON_AddStringToObject(c, "persona", S.c.persona);
  cJSON_AddStringToObject(c, "voice", S.c.voice);
  cJSON_AddStringToObject(c, "model", S.c.model);
  cJSON_AddNumberToObject(c, "volume", S.c.volume);
  cJSON_AddNumberToObject(c, "pause_s", S.c.pauseS);
  cJSON_AddNumberToObject(c, "resume_s", S.c.resumeS);
  cJSON_AddStringToObject(c, "backend_model", S.c.backendModel);
  cJSON_AddStringToObject(c, "backend_effort", S.c.backendEffort);
  cJSON_AddBoolToObject(c, "web_search", S.c.webSearch);
  cJSON_AddStringToObject(c, "knowledge", S.c.knowledge);
  cJSON_AddStringToObject(c, "wake", S.c.wake);
  cJSON_AddStringToObject(c, "lang", S.c.lang);
  cJSON_AddNumberToObject(c, "wake_sens", S.c.wakeSens);
  cJSON_AddBoolToObject(c, "wake_beep", S.c.wakeBeep);
  cJSON_AddStringToObject(c, "api_key", withMeta && S.c.apiKey[0] ? "********" : "");  // the key lives in NVS
  cJSON_AddNumberToObject(c, "budget_day", S.c.budgetDay);
  cJSON* nt = cJSON_AddObjectToObject(r, "night");
  cJSON_AddBoolToObject(nt, "on", S.c.nightOn);
  cJSON_AddNumberToObject(nt, "from", S.c.nightFrom);
  cJSON_AddNumberToObject(nt, "to", S.c.nightTo);
  cJSON_AddNumberToObject(nt, "brightness", S.c.nightBright);
  cJSON* h = cJSON_AddObjectToObject(r, "smarthome");
  cJSON_AddStringToObject(h, "mood_light", S.c.moodLight);
  cJSON_AddStringToObject(h, "url", S.c.haUrl);
  // the token never leaves the device again, the UI only learns whether one is set
  cJSON_AddStringToObject(h, "token", withMeta && S.c.haToken[0] ? "********" : "");  // in NVS too

  const eyes::Look& l = S.l;
  cJSON* k = cJSON_AddObjectToObject(r, "look");
  cJSON_AddNumberToObject(k, "colorway", l.colorway);
  colorOut(k, "sclera", l.sclera);
  colorOut(k, "iris", l.iris);
  colorOut(k, "accent", l.accent);
  colorOut(k, "shine", l.shine);
  colorOut(k, "bg", l.bg);
  cJSON_AddNumberToObject(k, "eye_w", l.eyeW);
  cJSON_AddNumberToObject(k, "eye_h", l.eyeH);
  cJSON_AddNumberToObject(k, "spacing", l.spacing);
  cJSON_AddNumberToObject(k, "pos_y", l.posY);
  cJSON_AddNumberToObject(k, "tilt", l.tilt);
  cJSON_AddNumberToObject(k, "iris_w", l.irisW);
  cJSON_AddNumberToObject(k, "iris_h", l.irisH);
  cJSON_AddNumberToObject(k, "iris_egg", l.irisEgg);
  cJSON_AddNumberToObject(k, "highlights", l.highlights);
  cJSON_AddNumberToObject(k, "stroke", l.stroke);
  cJSON_AddBoolToObject(k, "blush", l.blush);
  cJSON_AddBoolToObject(k, "wobble", l.wobble);
  cJSON_AddNumberToObject(k, "shape", l.shape);
  cJSON_AddNumberToObject(k, "pupil", l.pupil);
  cJSON_AddNumberToObject(k, "lid_top", l.lidTop);
  cJSON_AddNumberToObject(k, "lid_bottom", l.lidBottom);
  cJSON_AddNumberToObject(k, "brow", l.brow);
  cJSON_AddNumberToObject(k, "asym", l.asym);
  cJSON_AddNumberToObject(k, "squash", l.squash);

  cJSON* b = cJSON_AddObjectToObject(r, "behavior");
  cJSON_AddNumberToObject(b, "breath", l.breath);
  cJSON_AddNumberToObject(b, "blink_min", l.blinkMin);
  cJSON_AddNumberToObject(b, "blink_max", l.blinkMax);
  cJSON_AddNumberToObject(b, "gaze_activity", l.gazeActivity);
  cJSON_AddNumberToObject(b, "head_follow", l.headFollow);
  cJSON_AddNumberToObject(b, "idle_min", l.idleMin);
  cJSON_AddNumberToObject(b, "idle_max", l.idleMax);
  cJSON_AddNumberToObject(b, "sleep_after", l.sleepAfter);
  cJSON_AddNumberToObject(b, "energy", l.energy);
  cJSON_AddNumberToObject(b, "roam", l.roam);
  cJSON_AddNumberToObject(b, "cheer", l.cheer);
  cJSON_AddNumberToObject(b, "cheek", l.cheek);
  cJSON_AddNumberToObject(b, "sleepy", l.sleepy);
  cJSON_AddNumberToObject(b, "shy", l.shy);
  cJSON_AddNumberToObject(b, "grumpy", l.grumpy);
  cJSON_AddBoolToObject(b, "music", l.music);

  cJSON* d = cJSON_AddObjectToObject(r, "display");
  cJSON_AddNumberToObject(d, "brightness", S.brightness);
  cJSON_AddNumberToObject(d, "fps", S.fps);
  cJSON_AddNumberToObject(d, "leds", S.ledMode);
  cJSON_AddNumberToObject(d, "led_brightness", S.ledBright);
  colorOut(d, "led_color", S.ledColor);
  cJSON_AddNumberToObject(d, "off_min", S.offMin);
  cJSON_AddBoolToObject(d, "schedule_on", S.schedOn);
  cJSON* sc = cJSON_AddArrayToObject(d, "schedule");
  for (int i = 0; i < S.schedN; ++i) {
    char at[24];
    snprintf(at, sizeof(at), "%02d:%02d", S.schedAt[i] / 60, S.schedAt[i] % 60);
    cJSON* o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "at", at);
    cJSON_AddNumberToObject(o, "b", S.schedB[i]);
    cJSON_AddNumberToObject(o, "v", S.schedV[i]);
    cJSON_AddItemToArray(sc, o);
  }

  cJSON* up = cJSON_AddObjectToObject(r, "update");
  cJSON_AddBoolToObject(up, "auto", S.upAuto);
  char upAt[24];
  snprintf(upAt, sizeof(upAt), "%02d:%02d", S.upAt / 60, S.upAt % 60);
  cJSON_AddStringToObject(up, "at", upAt);

  cJSON* cu = cJSON_AddArrayToObject(r, "custom");
  for (int i = 0; i < S.cuN; ++i) {
    cJSON* o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "name", S.cuName[i]);
    cJSON* st = cJSON_AddArrayToObject(o, "steps");
    for (int k = 0; k < S.cuLen[i]; ++k) cJSON_AddItemToArray(st, cJSON_CreateString(S.cuStep[i][k]));
    cJSON_AddItemToArray(cu, o);
  }

  cJSON* a = cJSON_AddObjectToObject(r, "animations");
  for (int i = 0; i < eyes::behaviorCount() && i < 64; ++i) {
    int kind = eyes::behaviorKind(i);
    if (kind == eyes::K_INTERNAL) continue;
    cJSON* e = cJSON_AddObjectToObject(a, eyes::behaviorName(i));
    cJSON_AddBoolToObject(e, "on", S.on[i]);
    cJSON_AddNumberToObject(e, "speed", S.speed[i]);
    cJSON_AddNumberToObject(e, "intensity", S.intensity[i]);
    if (withMeta)
      cJSON_AddStringToObject(e, "kind", kind == eyes::K_IDLE    ? "idle"
                                         : kind == eyes::K_TOUCH ? "touch"
                                         : kind == eyes::K_ERROR ? "error"
                                         : kind == eyes::K_HOME  ? "home"
                                                                 : "reaction");
  }
  if (withMeta) {
    cJSON* m = cJSON_AddObjectToObject(r, "meta");
    cJSON* cw = cJSON_AddArrayToObject(m, "colorways");
    for (int i = 0; i < eyes::colorwayCount(); ++i) {
      cJSON* o = cJSON_CreateObject();
      cJSON_AddStringToObject(o, "name", eyes::colorwayNameAt(i));
      colorOut(o, "sclera", eyes::colorwayColor(i, 0));
      colorOut(o, "iris", eyes::colorwayColor(i, 1));
      cJSON_AddItemToArray(cw, o);
    }
    cJSON_AddItemToObject(m, "presets", cJSON_Parse(PRESETS_JSON));
    cJSON* wk = cJSON_AddArrayToObject(m, "wakes");  // the names to choose from: those this device can hear
    for (const Wake& w : WAKES) {
      if (w.model && !mic::hasWakeModel(w.model)) continue;
      cJSON* o = cJSON_CreateObject();
      cJSON_AddStringToObject(o, "id", w.id);
      cJSON_AddStringToObject(o, "name", w.name);
      cJSON_AddStringToObject(o, "phrase", w.phrase);
      cJSON_AddItemToArray(wk, o);
    }
    cJSON* lg = cJSON_AddArrayToObject(m, "langs");
    for (const i18n::Lang& l : i18n::LANGS) {
      cJSON* o = cJSON_CreateObject();
      cJSON_AddStringToObject(o, "code", l.code);
      cJSON_AddStringToObject(o, "name", l.name);
      cJSON_AddItemToArray(lg, o);
    }
    cJSON* ms = cJSON_AddArrayToObject(m, "models");
    for (const Model& md : MODELS) {
      cJSON* o = cJSON_CreateObject();
      cJSON_AddStringToObject(o, "id", md.id);
      cJSON_AddStringToObject(o, "label", md.label);
      cJSON_AddItemToArray(ms, o);
    }
    const char* voices[] = {"marin", "cedar", "juni", "flitz", "brise", "sillage", "alloy", "ash", "ballad", "coral",
                            "echo", "sage", "shimmer", "verse", "quartz", "ripple", "vesper", "willow", "stone",
                            "gleam", "meridian", "beacon", "delta", "cinder", "bossa", "tempo", "harema", "nira",
                            "noeul", "nuri", "shida"};
    cJSON* vv = cJSON_AddArrayToObject(m, "voices");
    for (auto v : voices) cJSON_AddItemToArray(vv, cJSON_CreateString(v));
    cJSON_AddBoolToObject(m, "sd", sd::ok());
  }
  return r;
}

static void merge(const cJSON* j) {
  const cJSON* c = cJSON_GetObjectItem(j, "conversation");
  if (c) {
    str(c, "name", S.c.name, sizeof(S.c.name));
    str(c, "persona", S.c.persona, sizeof(S.c.persona));
    str(c, "voice", S.c.voice, sizeof(S.c.voice));
    str(c, "model", S.c.model, sizeof(S.c.model));
    num(c, "volume", S.c.volume);
    num(c, "pause_s", S.c.pauseS);
    num(c, "resume_s", S.c.resumeS);
    str(c, "backend_model", S.c.backendModel, sizeof(S.c.backendModel));
    str(c, "backend_effort", S.c.backendEffort, sizeof(S.c.backendEffort));
    flag(c, "web_search", S.c.webSearch);
    str(c, "knowledge", S.c.knowledge, sizeof(S.c.knowledge));
    char was[12];
    snprintf(was, sizeof(was), "%s", S.c.wake);
    str(c, "wake", S.c.wake, sizeof(S.c.wake));
    snprintf(was, sizeof(was), "%s", S.c.lang);
    str(c, "lang", S.c.lang, sizeof(S.c.lang));
    int nl = i18n::index(S.c.lang);  // another language: the default personality follows, an own one stays
    if (strcmp(was, S.c.lang) && nl >= 0 && !cJSON_GetObjectItem(c, "persona"))
      for (const i18n::Lang& L : i18n::LANGS)
        if (!strcmp(S.c.persona, L.persona)) snprintf(S.c.persona, sizeof(S.c.persona), "%s", i18n::LANGS[nl].persona);
    if (!strcmp(S.c.wake, "both")) snprintf(S.c.wake, sizeof(S.c.wake), "pixel");     // up to 1.1: Pixel and
    if (!strcmp(S.c.wake, "halloesp")) snprintf(S.c.wake, sizeof(S.c.wake), "esp");   // Hallo ESP at once
    if (strcmp(was, S.c.wake) && !cJSON_GetObjectItem(c, "name"))  // another wake word: another name
      snprintf(S.c.name, sizeof(S.c.name), "%s", wakeOf(S.c.wake).name);
    num(c, "wake_sens", S.c.wakeSens);
    flag(c, "wake_beep", S.c.wakeBeep);
    const cJSON* k = cJSON_GetObjectItem(c, "api_key");
    if (cJSON_IsString(k) && strcmp(k->valuestring, "********")) str(c, "api_key", S.c.apiKey, sizeof(S.c.apiKey));
    num(c, "budget_day", S.c.budgetDay);
  }
  const cJSON* nt = cJSON_GetObjectItem(j, "night");
  if (nt) {
    flag(nt, "on", S.c.nightOn);
    num(nt, "from", S.c.nightFrom);
    num(nt, "to", S.c.nightTo);
    num(nt, "brightness", S.c.nightBright);
  }
  const cJSON* h = cJSON_GetObjectItem(j, "smarthome");
  if (h) {
    char oldUrl[sizeof(S.c.haUrl)];
    snprintf(oldUrl, sizeof(oldUrl), "%s", S.c.haUrl);
    str(h, "url", S.c.haUrl, sizeof(S.c.haUrl));
    str(h, "mood_light", S.c.moodLight, sizeof(S.c.moodLight));
    size_t n = strlen(S.c.haUrl);
    while (n && S.c.haUrl[n - 1] == '/') S.c.haUrl[--n] = 0;
    const cJSON* t = cJSON_GetObjectItem(h, "token");
    bool newToken = cJSON_IsString(t) && strcmp(t->valuestring, "********");
    if (newToken)
      str(h, "token", S.c.haToken, sizeof(S.c.haToken));
    else if (strcmp(oldUrl, S.c.haUrl))
      S.c.haToken[0] = 0;  // another server never gets the old token (it would be sent there)
  }
  eyes::Look& l = S.l;
  const cJSON* k = cJSON_GetObjectItem(j, "look");
  if (k) {
    num(k, "colorway", l.colorway);
    colorIn(k, "sclera", l.sclera);
    colorIn(k, "iris", l.iris);
    colorIn(k, "accent", l.accent);
    colorIn(k, "shine", l.shine);
    colorIn(k, "bg", l.bg);
    num(k, "eye_w", l.eyeW);
    num(k, "eye_h", l.eyeH);
    num(k, "spacing", l.spacing);
    num(k, "pos_y", l.posY);
    num(k, "tilt", l.tilt);
    num(k, "iris_w", l.irisW);
    num(k, "iris_h", l.irisH);
    num(k, "iris_egg", l.irisEgg);
    num(k, "highlights", l.highlights);
    num(k, "stroke", l.stroke);
    flag(k, "blush", l.blush);
    flag(k, "wobble", l.wobble);
    num(k, "shape", l.shape);
    num(k, "pupil", l.pupil);
    num(k, "lid_top", l.lidTop);
    num(k, "lid_bottom", l.lidBottom);
    num(k, "brow", l.brow);
    num(k, "asym", l.asym);
    num(k, "squash", l.squash);
  }
  const cJSON* b = cJSON_GetObjectItem(j, "behavior");
  if (b) {
    num(b, "breath", l.breath);
    num(b, "blink_min", l.blinkMin);
    num(b, "blink_max", l.blinkMax);
    num(b, "gaze_activity", l.gazeActivity);
    num(b, "head_follow", l.headFollow);
    num(b, "idle_min", l.idleMin);
    num(b, "idle_max", l.idleMax);
    num(b, "sleep_after", l.sleepAfter);
    num(b, "energy", l.energy);
    num(b, "roam", l.roam);
    num(b, "cheer", l.cheer);
    num(b, "cheek", l.cheek);
    num(b, "sleepy", l.sleepy);
    num(b, "shy", l.shy);
    num(b, "grumpy", l.grumpy);
    flag(b, "music", l.music);
  }
  const cJSON* d = cJSON_GetObjectItem(j, "display");
  if (d) {
    num(d, "brightness", S.brightness);
    num(d, "fps", S.fps);
    num(d, "leds", S.ledMode);
    num(d, "led_brightness", S.ledBright);
    colorIn(d, "led_color", S.ledColor);
    num(d, "off_min", S.offMin);
    flag(d, "schedule_on", S.schedOn);
    const cJSON* sc = cJSON_GetObjectItem(d, "schedule");
    if (cJSON_IsArray(sc)) {  // replaced as a whole
      S.schedN = 0;
      const cJSON* o;
      cJSON_ArrayForEach(o, sc) {
        const cJSON* at = cJSON_GetObjectItem(o, "at");
        int h, m, b = 100, v = -1;
        if (S.schedN >= 8 || !cJSON_IsString(at) || sscanf(at->valuestring, "%d:%d", &h, &m) != 2) continue;
        num(o, "b", b);
        num(o, "v", v);
        S.schedAt[S.schedN] = (((h * 60 + m) % 1440) + 1440) % 1440;
        S.schedV[S.schedN] = v < 0 ? -1 : v > 100 ? 100 : v;
        S.schedB[S.schedN++] = b < 0 ? 0 : b > 100 ? 100 : b;
      }
    }
  }
  const cJSON* up = cJSON_GetObjectItem(j, "update");
  if (up) {
    flag(up, "auto", S.upAuto);
    const cJSON* at = cJSON_GetObjectItem(up, "at");
    int hh, mm;
    if (cJSON_IsString(at) && sscanf(at->valuestring, "%d:%d", &hh, &mm) == 2 && hh >= 0 && hh < 24 && mm >= 0 && mm < 60)
      S.upAt = hh * 60 + mm;
  }
  const cJSON* cu = cJSON_GetObjectItem(j, "custom");
  if (cJSON_IsArray(cu)) {  // replaced as a whole
    S.cuN = 0;
    const cJSON* o;
    cJSON_ArrayForEach(o, cu) {
      const cJSON* n = cJSON_GetObjectItem(o, "name");
      if (S.cuN >= 8 || !cJSON_IsString(n) || !n->valuestring[0]) continue;
      snprintf(S.cuName[S.cuN], sizeof(S.cuName[0]), "%s", n->valuestring);
      int k = 0;
      const cJSON* st;
      cJSON_ArrayForEach(st, cJSON_GetObjectItem(o, "steps")) {
        if (k < 6 && cJSON_IsString(st) && eyes::behaviorIndex(st->valuestring) >= 0)
          snprintf(S.cuStep[S.cuN][k++], sizeof(S.cuStep[0][0]), "%s", st->valuestring);
      }
      S.cuLen[S.cuN] = k;
      if (k) S.cuN++;
    }
  }
  const cJSON* a = cJSON_GetObjectItem(j, "animations");
  const cJSON* e;
  cJSON_ArrayForEach(e, a) {
    int i = eyes::behaviorIndex(e->string);
    if (i < 0 || i >= 64) continue;
    flag(e, "on", S.on[i]);
    num(e, "speed", S.speed[i]);
    num(e, "intensity", S.intensity[i]);
  }
  // sanity
  if (S.c.volume < 0) S.c.volume = 0;
  if (S.c.volume > 100) S.c.volume = 100;
  if (S.brightness < 5) S.brightness = 5;
  if (S.brightness > 100) S.brightness = 100;
  if (S.offMin < 0) S.offMin = 0;
  if (S.offMin > 240) S.offMin = 240;
  if (S.fps < 10) S.fps = 10;
  if (S.fps > 60) S.fps = 60;
  if (S.ledMode < 0 || S.ledMode > 2) S.ledMode = 1;
  if (S.ledBright < 0) S.ledBright = 0;
  if (S.ledBright > 100) S.ledBright = 100;
  if (S.c.pauseS < 1.5f) S.c.pauseS = 1.5f;
  snprintf(S.c.wake, sizeof(S.c.wake), "%s", wakeOf(S.c.wake).id);  // unknown: Pixel
  if (i18n::index(S.c.lang) < 0) snprintf(S.c.lang, sizeof(S.c.lang), "en");
  if (S.c.wakeSens < 0.f) S.c.wakeSens = 0.f;
  if (S.c.wakeSens > 1.f) S.c.wakeSens = 1.f;
  S.c.nightFrom = ((S.c.nightFrom % 24) + 24) % 24;
  S.c.nightTo = ((S.c.nightTo % 24) + 24) % 24;
  if (S.c.nightBright < 5) S.c.nightBright = 5;
  if (S.c.nightBright > 100) S.c.nightBright = 100;
  if (S.c.budgetDay < 0) S.c.budgetDay = 0;
  if (strncmp(S.c.model, "gpt-live", 8) && strncmp(S.c.model, "gpt-realtime", 12))  // e.g. old Gemini names
    snprintf(S.c.model, sizeof(S.c.model), "%s", CFG_LIVE_MODEL);
  if (!S.c.backendModel[0]) snprintf(S.c.backendModel, sizeof(S.c.backendModel), "%s", CFG_BACKEND_MODEL);
  if (!S.c.backendEffort[0]) snprintf(S.c.backendEffort, sizeof(S.c.backendEffort), "%s", CFG_BACKEND_EFFORT);
  bool known = false;  // Gemini voice names (Puck, Kore ...) do not exist here
  for (const char* v : {"marin", "cedar", "juni", "flitz", "brise", "sillage", "alloy", "ash", "ballad", "coral", "echo",
                        "sage", "shimmer", "verse", "quartz", "ripple", "vesper", "willow", "stone", "gleam", "meridian",
                        "beacon", "delta", "cinder", "bossa", "tempo", "harema", "nira", "noeul", "nuri", "shida"})
    known |= !strcmp(v, S.c.voice);
  if (!known) snprintf(S.c.voice, sizeof(S.c.voice), "%s", CFG_LIVE_VOICE);
  stripOldKnowledge(S.c.knowledge);  // the old self description would repeat the built-in one
}

// ---------------------------------------------------------------- storage
// The OpenAI key and the Home Assistant token stay in the internal flash (NVS), not in config.json on
// the SD card: a card that is taken out, copied or lent must not carry them.
static void saveKeys() {
  static char lastKey[sizeof(S.c.apiKey)] = "\x01", lastTok[sizeof(S.c.haToken)] = "\x01";
  if (!strcmp(lastKey, S.c.apiKey) && !strcmp(lastTok, S.c.haToken)) return;  // no flash wear for nothing
  nvs_handle_t h;
  if (nvs_open("pixel_keys", NVS_READWRITE, &h) != ESP_OK) return;
  nvs_set_str(h, "openai", S.c.apiKey);
  nvs_set_str(h, "ha", S.c.haToken);
  nvs_commit(h);
  nvs_close(h);
  memcpy(lastKey, S.c.apiKey, sizeof(lastKey));
  memcpy(lastTok, S.c.haToken, sizeof(lastTok));
}
static void loadKeys() {
  nvs_handle_t h;
  if (nvs_open("pixel_keys", NVS_READONLY, &h) != ESP_OK) return;  // nothing stored yet (or an old config.json)
  size_t n = sizeof(S.c.apiKey);
  nvs_get_str(h, "openai", S.c.apiKey, &n);
  n = sizeof(S.c.haToken);
  nvs_get_str(h, "ha", S.c.haToken, &n);
  nvs_close(h);
}

static void save() {
  cJSON* r = build(false);
  char* txt = cJSON_Print(r);
  cJSON_Delete(r);
  bool ok = false;
  if (sd::ok()) {
    FILE* f = fopen(PATH, "w");
    if (f) {
      ok = fputs(txt, f) >= 0;
      fclose(f);
    }
  }
  if (!ok) {  // no SD card: keep it in NVS
    nvs_handle_t h;
    if (nvs_open("pixel", NVS_READWRITE, &h) == ESP_OK) {
      nvs_set_blob(h, "config", txt, strlen(txt) + 1);
      nvs_commit(h);
      nvs_close(h);
    }
  }
  cJSON_free(txt);
  saveKeys();
}

static char* loadText() {
  if (sd::ok()) {
    FILE* f = fopen(PATH, "r");
    if (f) {
      fseek(f, 0, SEEK_END);
      long n = ftell(f);
      fseek(f, 0, SEEK_SET);
      char* t = (char*)heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM);
      n = fread(t, 1, n, f);
      t[n] = 0;
      fclose(f);
      return t;
    }
  }
  nvs_handle_t h;
  size_t n = 0;
  if (nvs_open("pixel", NVS_READONLY, &h) != ESP_OK) return nullptr;
  char* t = nullptr;
  if (nvs_get_blob(h, "config", nullptr, &n) == ESP_OK && n > 0) {
    t = (char*)heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    nvs_get_blob(h, "config", t, &n);
  }
  nvs_close(h);
  return t;
}

void init() {
  mtx = xSemaphoreCreateMutex();
  esp_err_t e = nvs_flash_init();  // (the WiFi start does the same later; twice is fine)
  if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    nvs_flash_erase();
    nvs_flash_init();
  }
  defaults();
  char* t = loadText();
  if (t) {
    cJSON* j = cJSON_Parse(t);
    if (j) {
      merge(j);  // an old config.json may still hold the keys: they move to NVS with the save below
      // set up before there was a choice of language: it spoke German
      if (!cJSON_GetObjectItem(cJSON_GetObjectItem(j, "conversation"), "lang")) snprintf(S.c.lang, sizeof(S.c.lang), "de");
      cJSON_Delete(j);
      ESP_LOGI(TAG, "settings loaded from %s", sd::ok() ? PATH : "NVS");
    }
    free(t);
  }
  loadKeys();
  // a key built into the firmware (secrets.h) moves into NVS with the save below: an update from GitHub (built
  // without secrets.h) still has it
  if (!S.c.apiKey[0] && OPENAI_API_KEY[0]) snprintf(S.c.apiKey, sizeof(S.c.apiKey), "%s", OPENAI_API_KEY);
  save();  // the defaults, or the file without the keys
}

uint32_t version() { return ver; }

void look(eyes::Look& out) {
  xSemaphoreTake(mtx, portMAX_DELAY);
  out = S.l;
  xSemaphoreGive(mtx);
}
void conv(Conv& out) {
  xSemaphoreTake(mtx, portMAX_DELAY);
  out = S.c;
  xSemaphoreGive(mtx);
  // the persona may refer to the name
  char* p = strstr(out.persona, "{name}");
  if (p) {
    char* tmp = (char*)heap_caps_malloc(sizeof(out.persona), MALLOC_CAP_SPIRAM);
    snprintf(tmp, sizeof(out.persona), "%.*s%s%s", (int)(p - out.persona), out.persona, out.name, p + 6);
    snprintf(out.persona, sizeof(out.persona), "%s", tmp);
    free(tmp);
  }
}
const char* lang() { return S.c.lang; }
const char* apiKey() { return S.c.apiKey[0] ? S.c.apiKey : OPENAI_API_KEY; }
int brightness() { return S.brightness; }
void leds(int& mode, int& bright, uint32_t& color) {
  mode = S.ledMode;
  bright = S.ledBright;
  color = S.ledColor;
}
int fps() { return S.fps; }
int displayOffMin() { return S.offMin; }
bool updateAuto() { return S.upAuto; }
int updateAt() { return S.upAt; }
int customCount() { return S.cuN; }
const char* customName(int i) { return i >= 0 && i < S.cuN ? S.cuName[i] : ""; }
int custom(const char* name, int* beh) {
  for (int i = 0; i < S.cuN; ++i)
    if (!strcasecmp(name, S.cuName[i])) {
      for (int k = 0; k < S.cuLen[i]; ++k) beh[k] = eyes::behaviorIndex(S.cuStep[i][k]);
      return S.cuLen[i];
    }
  return 0;
}

// ---- backup of the settings on the SD card (before every firmware update); the keys are not in it
static const char* BACKUP = "/sdcard/pixel/config.backup.json";
bool backup(const char* fw) {
  if (!sd::ok()) return false;
  xSemaphoreTake(mtx, portMAX_DELAY);
  cJSON* r = cJSON_CreateObject();
  cJSON_AddNumberToObject(r, "saved", (double)time(nullptr));
  cJSON_AddStringToObject(r, "build", fw);
  cJSON_AddItemToObject(r, "config", build(false));
  xSemaphoreGive(mtx);
  char* t = cJSON_PrintUnformatted(r);
  cJSON_Delete(r);
  FILE* f = fopen(BACKUP, "w");
  bool ok = f && fputs(t, f) >= 0;
  if (f) fclose(f);
  cJSON_free(t);
  return ok;
}
bool backupInfo(long& saved, char* build, int cap) {
  FILE* f = sd::ok() ? fopen(BACKUP, "r") : nullptr;
  if (!f) return false;
  char head[160] = "";
  fread(head, 1, sizeof(head) - 1, f);
  fclose(f);
  const char* s = strstr(head, "\"saved\":");
  const char* b = strstr(head, "\"build\":\"");
  saved = s ? atol(s + 8) : 0;
  if (b) snprintf(build, cap, "%.*s", (int)(strchr(b + 9, '"') ? strchr(b + 9, '"') - (b + 9) : 0), b + 9);
  return true;
}
bool restoreBackup() {
  FILE* f = sd::ok() ? fopen(BACKUP, "r") : nullptr;
  if (!f) return false;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  char* t = (char*)heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM);
  bool ok = t && fread(t, 1, n, f) == (size_t)n;
  fclose(f);
  cJSON* j = ok ? (t[n] = 0, cJSON_Parse(t)) : nullptr;
  free(t);
  cJSON* c = cJSON_GetObjectItem(j, "config");
  if (c) {  // the keys stay as they are (the backup holds empty ones)
    cJSON_DeleteItemFromObject(cJSON_GetObjectItem(c, "conversation"), "api_key");
    cJSON_DeleteItemFromObject(cJSON_GetObjectItem(c, "smarthome"), "token");
    xSemaphoreTake(mtx, portMAX_DELAY);
    merge(c);
    save();
    ver = ver + 1;
    xSemaphoreGive(mtx);
  }
  if (j) cJSON_Delete(j);
  return c != nullptr;
}
bool scheduled(int minute, int& bright, int& volume) {
  int i = S.schedOn ? sched::current(S.schedAt, S.schedN, minute) : -1;
  if (i < 0) return false;
  bright = S.schedB[i];
  volume = S.schedV[i];
  return true;
}
bool anim(int i, bool& on, float& speed, float& intensity) {
  if (i < 0 || i >= 64) return false;
  on = S.on[i];
  speed = S.speed[i];
  intensity = S.intensity[i];
  return true;
}

char* toJson() {
  xSemaphoreTake(mtx, portMAX_DELAY);
  cJSON* r = build(true);
  xSemaphoreGive(mtx);
  char* t = cJSON_PrintUnformatted(r);
  cJSON_Delete(r);
  return t;
}

bool apply(const char* json) {
  cJSON* j = cJSON_Parse(json);
  if (!j) return false;
  xSemaphoreTake(mtx, portMAX_DELAY);
  merge(j);
  save();
  ver = ver + 1;
  xSemaphoreGive(mtx);
  cJSON_Delete(j);
  return true;
}

void factory() {
  xSemaphoreTake(mtx, portMAX_DELAY);
  defaults();
  save();
  ver = ver + 1;
  xSemaphoreGive(mtx);
}

void reset() {
  xSemaphoreTake(mtx, portMAX_DELAY);
  char key[sizeof(S.c.apiKey)];  // the key from the setup survives, or the robot could not talk any more
  memcpy(key, S.c.apiKey, sizeof(key));
  defaults();
  memcpy(S.c.apiKey, key, sizeof(key));
  save();
  ver = ver + 1;
  xSemaphoreGive(mtx);
}

}  // namespace settings
