// Voice conversation over OpenAI. GPT-Live: after the wake word the microphone (buffered from the wake word on) is
// streamed as 16 kHz PCM, the reply comes back as a continuous 16 kHz stream that is played
// as it arrives. The WebSocket is opened ahead of time and kept warm while idle (an idle
// socket is not billed), so a session starts ~0.4 s after the wake word.
// The voice model delegates tools to a cheap text model ("Responses delegation"): web search,
// change_look and smart_home. Transcript triggers (Decisions) pick the eye animation while
// the robot talks and the listening mood while the person talks.
// After pause_s of quiet the session is closed; the wake word within resume_s starts a new
// one that gets the conversation so far as text history.
//
// Or the Realtime API (gpt-realtime-2.1, chosen in the web interface): 24 kHz audio (resampled on the
// fly), the model calls the tools itself, web search runs through the cheap text model from here, the
// eye reaction comes from the Decisions API on what it says, and the session stays open between
// conversations (cached instructions, see sendStartRealtime).
// Tools: look, animations, smart home / lamps, web search, timers, memory, weather, self management.
#include "live.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <time.h>
#include <sys/stat.h>

#include <initializer_list>

#include "cJSON.h"
#include "config.h"
#include "esp_attr.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "esp_websocket_client.h"
#include "eyes.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "assist.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "guard.h"
#include "ha.h"
#include "mbedtls/base64.h"
#include "mic.h"
#include "net.h"
#include "sd.h"
#include "creds.h"
#include "settings.h"
#include "battery.h"
#include "update.h"
#include "version.h"

namespace live {

static const char* TAG = "live";
static const char* URL = "wss://api.openai.com/v1/live/sessions";

static esp_websocket_client_handle_t ws = nullptr;
static volatile bool wsUp = false;        // upgraded and usable
static volatile int handshakeStatus = 0;  // HTTP status of a rejected upgrade (401, 429, ...)
static volatile bool started = false;     // session.started
static bool wasStarted = false;            // the last session did start (for the false wake-up check)
static volatile bool closedEv = false;    // session.closed
static char startError[48];               // error code before session.started
static volatile Phase ph = IDLE;
static volatile bool userSpokeFlag = false;
static TaskHandle_t taskH = nullptr;
static QueueHandle_t evQ = nullptr;
static bool rt = false;                   // Realtime API instead of GPT-Live
static char wsModel[64] = "";             // model the (warm) socket was opened for
static int64_t wsOpenedUs = 0;
static volatile bool rtResponding = false;  // Realtime: a response is being generated
static bool rtHasConv = false;              // Realtime: the open session holds the conversation so far
static char rtSessVoice[24] = "";           // ... and speaks with this voice

EXT_RAM_BSS_ATTR static settings::Conv cv;  // settings snapshot of the running conversation

// ------------------------------------------------------------------ eye events
static void post(int kind, const char* name, uint32_t rgb = 0) {
  Event e;
  e.kind = kind;
  e.rgb = rgb;
  snprintf(e.name, sizeof(e.name), "%s", name);
  if (evQ) xQueueSend(evQ, &e, 0);
}

// ------------------------------------------------------------------ text helpers
// cut a trailing, incomplete UTF-8 sequence after truncating
static void utf8Trim(char* s) {
  int len = strlen(s), i = len;
  while (i > 0 && ((uint8_t)s[i - 1] & 0xC0) == 0x80) --i;
  if (i == 0) return;
  uint8_t lead = s[i - 1];
  int need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
  if (len - (i - 1) < need) s[i - 1] = 0;
}
static void utf8Cat(char* dst, size_t cap, const char* src) {
  strncat(dst, src, cap - strlen(dst) - 1);
  utf8Trim(dst);
}

static int b64decode(const char* in, int n, uint8_t* out, int cap) {
  static int8_t T[256];
  if (!T['B']) {
    memset(T, -1, sizeof(T));
    const char* a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (int i = 0; i < 64; ++i) T[(uint8_t)a[i]] = i;
  }
  int o = 0, bits = 0;
  uint32_t acc = 0;
  for (int i = 0; i < n && o < cap; ++i) {
    int v = T[(uint8_t)in[i]];
    if (v < 0) {
      if (in[i] == '=') break;
      continue;
    }
    acc = (acc << 6) | v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out[o++] = (uint8_t)(acc >> bits);
    }
  }
  return o;
}

// streaming polyphase resampler (up L, windowed-sinc low pass, down M): 16 <-> 24 kHz for Realtime.
// Position and phase advance incrementally - no divisions per sample (64-bit ones are library calls).
struct Resampler {
  static const int TAPS = 96, HIST = 64;  // HIST: power of two >= TAPS / L
  int L = 1, M = 1;
  float h[TAPS];
  float x[HIST];
  uint32_t n = 0;   // input samples seen
  uint32_t pk = 0;  // input index of the next output
  int pf = 0;       // its phase (0..L-1) at the up-sampled rate
  void init(int up, int down, float cutoffHz, float inRate) {
    L = up;
    M = down;
    const float fc = cutoffHz / (inRate * L), PI_ = 3.14159265f;
    float sum = 0;
    for (int j = 0; j < TAPS; ++j) {
      float t = j - (TAPS - 1) / 2.f;
      float sn = t == 0 ? 2 * fc : sinf(2 * PI_ * fc * t) / (PI_ * t);
      float w = 0.42f - 0.5f * cosf(2 * PI_ * j / (TAPS - 1)) + 0.08f * cosf(4 * PI_ * j / (TAPS - 1));
      h[j] = sn * w;
      sum += h[j];
    }
    for (int j = 0; j < TAPS; ++j) h[j] *= L / sum;
    reset();
  }
  void reset() {
    memset(x, 0, sizeof(x));
    n = pk = 0;
    pf = 0;
  }
  int run(const int16_t* in, int cnt, int16_t* out) {
    int o = 0;
    for (int i = 0; i < cnt; ++i) {
      x[n & (HIST - 1)] = in[i];
      ++n;
      while (pk < n) {  // the output at up-sampled time pk*L+pf needs inputs up to pk
        float y = 0;
        uint32_t idx = pk;
        for (int j = pf; j < TAPS; j += L, --idx) y += h[j] * x[idx & (HIST - 1)];  // older samples are 0 at start
        out[o++] = (int16_t)fmaxf(-32768.f, fminf(32767.f, y));
        pf += M;
        while (pf >= L) {
          pf -= L;
          ++pk;
        }
      }
    }
    return o;
  }
};
EXT_RAM_BSS_ATTR static Resampler upRs;
EXT_RAM_BSS_ATTR static Resampler downRs;

// ------------------------------------------------------------------ conversation history
// kept as text between sessions (input of the next session) and logged to the SD card
struct Msg {
  bool user;
  char text[420];
};
static const int HIST = 16;
EXT_RAM_BSS_ATTR static Msg hist[HIST];
static int histN = 0;
static int64_t pausedAtUs = -1000000000LL;
EXT_RAM_BSS_ATTR static char inText[420];
EXT_RAM_BSS_ATTR static char outText[420];  // the turn being spoken (both can grow at once: full duplex)
static SemaphoreHandle_t txtMtx = nullptr;

static void logLine(const char* who, const char* text) {
  time_t now = time(nullptr);
  struct tm tmv;
  localtime_r(&now, &tmv);
  static char line[520];
  snprintf(line, sizeof(line), "%04d-%02d-%02d %02d:%02d:%02d  %s: %s\n", tmv.tm_year + 1900, tmv.tm_mon + 1,
           tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec, who, text);
  sd::append("/sdcard/pixel/chat.log", line);
}
static void pushTurn(bool user, char* text) {
  // drop non-verbal markers like [hum] or [sniff]
  for (char* o = strchr(text, '['); o; o = strchr(o, '[')) {
    char* c = strchr(o, ']');
    if (!c) {
      *o = 0;
      break;
    }
    memmove(o, c + 1, strlen(c + 1) + 1);
  }
  // trim spaces
  char* t = text;
  while (*t == ' ') ++t;
  if (!*t) {
    text[0] = 0;
    return;
  }
  printf("%s \"%s\"\n", user ? "you:  " : "robot:", t);
  logLine(user ? "Du" : cv.name, t);
  if (histN == HIST) {
    memmove(&hist[0], &hist[1], sizeof(Msg) * (HIST - 1));
    histN--;
  }
  hist[histN].user = user;
  snprintf(hist[histN].text, sizeof(hist[0].text), "%s", t);
  utf8Trim(hist[histN].text);
  histN++;
  text[0] = 0;
}

// ------------------------------------------------------------------ live animation choice
// A trigger result arrives for every transcript fragment (~5 per second). Per turn each
// reaction is shown once, at most 3, and only confident choices count.
static int64_t lastOutUs = 0, lastInUs = 0;
static char turnAnims[3][16];
static int turnAnimN = 0;
static bool turnSituation = false;

static void newTurn() {
  turnAnimN = 0;
  turnSituation = false;
}

static void applyAnswers(const cJSON* ans, bool listening) {
  const cJSON* a;
  cJSON_ArrayForEach(a, ans) {
    const cJSON* nm = cJSON_GetObjectItem(a, "name");
    const cJSON* ch = cJSON_GetObjectItem(a, "choice");
    const cJSON* cf = cJSON_GetObjectItem(a, "confidence");
    if (!cJSON_IsString(nm) || !cJSON_IsString(ch)) continue;
    float conf = cJSON_IsNumber(cf) ? (float)cf->valuedouble : 0.f;
    const char* c = ch->valuestring;
    if (listening) {
      if (!strcmp(nm->valuestring, "mood") && strcmp(c, "none") && conf >= 0.4f) post(EV_MOOD, c);
    } else if (!strcmp(nm->valuestring, "situation")) {
      if (!turnSituation && conf >= 0.55f && strcmp(c, "normal")) {
        turnSituation = true;
        post(EV_ERROR, !strcmp(c, "cannot") ? "cant" : "huh");
        printf("eyes: %s (%.2f)\n", c, conf);
      }
    } else if (!strcmp(nm->valuestring, "anim")) {
      if (turnSituation || !strcmp(c, "none") || conf < 0.4f || turnAnimN >= 3) continue;
      bool seen = false;
      for (int i = 0; i < turnAnimN; ++i) seen |= !strcmp(turnAnims[i], c);
      if (seen) continue;
      snprintf(turnAnims[turnAnimN++], sizeof(turnAnims[0]), "%s", c);
      post(EV_REACT, c);
      printf("eyes: %s (%.2f)\n", c, conf);
    }
  }
}
static void onDecision(const cJSON* ev) {
  const cJSON* src = cJSON_GetObjectItem(ev, "source_event_type");
  const cJSON* ans = cJSON_GetObjectItem(cJSON_GetObjectItem(cJSON_GetObjectItem(ev, "event"), "decision"), "answers");
  applyAnswers(ans, cJSON_IsString(src) && strstr(src->valuestring, "input_transcript"));
}

// ------------------------------------------------------------------ backend (delegated work)
struct ToolCall {
  char id[64], name[24];
  char* args;  // JSON, PSRAM
};
static ToolCall calls[6];
static volatile int callW = 0, callR = 0;
static volatile int delegations = 0;  // delegated tasks still running
static volatile bool respHadCall = false;
static bool restartVoice = false;  // the voice changed: continue the conversation in a new session
static bool endReq = false;        // the person wants to stop talking (end_conversation)
static bool noReply = false;       // a tool result that must not start a new answer
static bool heardVoice = false;    // the microphone really heard someone in this session (or text was typed)
static bool byWake = true;         // this session was started by the wake word (not typed / web)
static char pendingSys[24] = "";   // self management to do once the robot has said its sentence
static int64_t pendingSysAt = 0;
static volatile bool interruptReq = false;
static char typed[400];            // typed in the web interface
static volatile bool typedPending = false;
static volatile bool weatherReq = false;
static volatile bool forgetFlag = false;

static void onResponseEvent(const cJSON* ev) {
  const cJSON* t = cJSON_GetObjectItem(ev, "type");
  if (!cJSON_IsString(t)) return;
  const char* ty = t->valuestring;
  if (!strcmp(ty, "response.created")) {
    respHadCall = false;
  } else if (!strcmp(ty, "response.output_item.done")) {
    const cJSON* it = cJSON_GetObjectItem(ev, "item");
    const cJSON* it_t = cJSON_GetObjectItem(it, "type");
    if (!cJSON_IsString(it_t)) return;
    if (!strcmp(it_t->valuestring, "function_call")) {
      const cJSON* id = cJSON_GetObjectItem(it, "call_id");
      const cJSON* nm = cJSON_GetObjectItem(it, "name");
      const cJSON* ar = cJSON_GetObjectItem(it, "arguments");
      if (!cJSON_IsString(id) || !cJSON_IsString(nm) || callW - callR >= 6) return;
      ToolCall& c = calls[callW % 6];
      snprintf(c.id, sizeof(c.id), "%s", id->valuestring);
      snprintf(c.name, sizeof(c.name), "%s", nm->valuestring);
      c.args = nullptr;
      if (cJSON_IsString(ar)) {
        c.args = (char*)heap_caps_malloc(strlen(ar->valuestring) + 1, MALLOC_CAP_SPIRAM);
        strcpy(c.args, ar->valuestring);
      }
      respHadCall = true;
      callW = callW + 1;
      printf("backend: %s %s\n", c.name, c.args ? c.args : "");
    } else if (!strcmp(it_t->valuestring, "web_search_call")) {
      printf("backend: web search\n");
    } else if (!strcmp(it_t->valuestring, "message")) {
      const cJSON* tx = cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(it, "content"), 0), "text");
      if (cJSON_IsString(tx)) printf("backend: \"%.200s\"\n", tx->valuestring);
    }
  } else if (!strcmp(ty, "response.completed") || !strcmp(ty, "response.failed") ||
             !strcmp(ty, "response.incomplete")) {
    bool failed = strcmp(ty, "response.completed") != 0;
    if (failed) ESP_LOGW(TAG, "backend %s", ty);
    if (!respHadCall || failed) {  // the delegated task is finished (otherwise it continues after the tool)
      if (delegations > 0) delegations = delegations - 1;
      if (delegations == 0) post(EV_BUSY, "0");
    }
  }
}

// ------------------------------------------------------------------ helper (Realtime mode)
// One kept-alive HTTPS connection to api.openai.com for the Decisions API (eye reaction to what
// the robot says) and the web search tool (cheap text model with hosted web search).
enum { JOB_DECIDE, JOB_SEARCH, JOB_WARM, JOB_CLOSE };
struct Job {
  int kind;
  char callId[64];
  char text[1000];
};
static QueueHandle_t jobQ = nullptr;
static volatile bool searchReady = false;
static char searchCall[64];
static char* searchOut = nullptr;  // PSRAM
static const int SEARCH_CAP = 2000;
static cJSON* decideQuestions = nullptr;
static const char* TRIGGER_OUT_REF;  // defined below

static int apiPost(esp_http_client_handle_t& h, const char* path, const char* body, char* resp, int cap) {
  resp[0] = 0;
  char url[96];
  snprintf(url, sizeof(url), "https://api.openai.com%s", path);
  for (int attempt = 0; attempt < 2; ++attempt) {
    if (!h) {
      esp_http_client_config_t hc = {};
      hc.url = url;
      hc.method = HTTP_METHOD_POST;
      hc.timeout_ms = 30000;
      hc.crt_bundle_attach = esp_crt_bundle_attach;
      hc.keep_alive_enable = true;
      hc.buffer_size = 4096;
      h = esp_http_client_init(&hc);
      static char auth[220];
      snprintf(auth, sizeof(auth), "Bearer %s", settings::apiKey());
      esp_http_client_set_header(h, "Authorization", auth);
      esp_http_client_set_header(h, "Content-Type", "application/json");
    } else {
      esp_http_client_set_url(h, url);
    }
    int len = strlen(body);
    if (esp_http_client_open(h, len) != ESP_OK || esp_http_client_write(h, body, len) < 0 ||
        esp_http_client_fetch_headers(h) < 0) {
      esp_http_client_cleanup(h);  // stale keep-alive connection: once more on a fresh one
      h = nullptr;
      continue;
    }
    resp[0] = 0;
    int st = esp_http_client_get_status_code(h), got = 0, r;
    while (got < cap - 1 && (r = esp_http_client_read(h, resp + got, cap - 1 - got)) > 0) got += r;
    resp[got] = 0;
    if (!esp_http_client_is_complete_data_received(h)) {
      esp_http_client_cleanup(h);
      h = nullptr;
    }
    return st;
  }
  return -1;
}

static void helperTask(void*) {
  esp_http_client_handle_t h = nullptr;
  const int CAP = 96 * 1024;
  char* resp = (char*)heap_caps_malloc(CAP, MALLOC_CAP_SPIRAM);
  EXT_RAM_BSS_ATTR static Job j;
  for (;;) {
    xQueueReceive(jobQ, &j, portMAX_DELAY);
    cJSON* b = cJSON_CreateObject();
    if (j.kind == JOB_CLOSE) {  // between sessions the connection would only hold memory
      if (h) esp_http_client_cleanup(h);
      h = nullptr;
      cJSON_Delete(b);
      continue;
    }
    if (j.kind == JOB_WARM) {  // open the TLS connection ahead of the first real request
      cJSON_Delete(b);
      int st = apiPost(h, "/v1/decisions", "{\"model\":\"gpt-6-luna\",\"input\":\"Hallo\",\"questions\":"
                       "[{\"type\":\"predicate\",\"name\":\"w\",\"instructions\":\"Ist das eine Begruessung?\"}]}",
                       resp, CAP);
      ESP_LOGI(TAG, "helper connection ready (HTTP %d)", st);
      continue;
    }
    if (j.kind == JOB_DECIDE) {
      if (!decideQuestions) {
        cJSON* t = cJSON_Parse(TRIGGER_OUT_REF);
        decideQuestions = cJSON_DetachItemFromObject(cJSON_GetObjectItem(t, "decisions"), "questions");
        cJSON_Delete(t);
      }
      char in[1100];
      snprintf(in, sizeof(in), "Pixel sagt: %s", j.text);
      cJSON_AddStringToObject(b, "model", "gpt-6-luna");
      cJSON_AddStringToObject(b, "input", in);
      cJSON_AddItemToObject(b, "questions", cJSON_Duplicate(decideQuestions, true));
    } else {
      cJSON_AddStringToObject(b, "model", cv.backendModel);
      if (cv.backendEffort[0]) cJSON_AddStringToObject(cJSON_AddObjectToObject(b, "reasoning"), "effort", cv.backendEffort);
      cJSON* w = cJSON_CreateObject();
      cJSON_AddStringToObject(w, "type", "web_search");
      cJSON_AddItemToArray(cJSON_AddArrayToObject(b, "tools"), w);
      cJSON_AddStringToObject(b, "instructions",
                              "Recherchiere im Web und antworte extrem knapp auf Deutsch mit den wichtigsten Fakten in "
                              "ein bis zwei Saetzen, so wie man sie spricht. Keine Listen, kein Markdown, keine Links, "
                              "keine englischen Woerter ausser Eigennamen.");
      cJSON_AddStringToObject(b, "input", j.text);
    }
    char* body = cJSON_PrintUnformatted(b);
    cJSON_Delete(b);
    int64_t t0 = esp_timer_get_time();
    int st = apiPost(h, j.kind == JOB_DECIDE ? "/v1/decisions" : "/v1/responses", body, resp, CAP);
    cJSON_free(body);
    cJSON* r = st == 200 ? cJSON_Parse(resp) : nullptr;
    if (st != 200) ESP_LOGW(TAG, "%s: HTTP %d %.120s", j.kind == JOB_DECIDE ? "decide" : "search", st, resp);
    if (j.kind == JOB_DECIDE) {
      if (r) applyAnswers(cJSON_GetObjectItem(r, "answers"), false);
    } else {
      searchOut[0] = 0;
      const cJSON* o;
      cJSON_ArrayForEach(o, cJSON_GetObjectItem(r, "output")) {
        const cJSON* ty = cJSON_GetObjectItem(o, "type");
        if (!cJSON_IsString(ty) || strcmp(ty->valuestring, "message")) continue;
        const cJSON* c;
        cJSON_ArrayForEach(c, cJSON_GetObjectItem(o, "content")) {
          const cJSON* tx = cJSON_GetObjectItem(c, "text");
          if (cJSON_IsString(tx)) utf8Cat(searchOut, SEARCH_CAP, tx->valuestring);
        }
      }
      if (!searchOut[0]) snprintf(searchOut, SEARCH_CAP, "Die Suche hat gerade nicht geklappt.");
      printf("web search (%d ms): \"%.200s\"\n", (int)((esp_timer_get_time() - t0) / 1000), searchOut);
      assist::addCost(CFG_PRICE_SEARCH);
      snprintf(searchCall, sizeof(searchCall), "%s", j.callId);
      searchReady = true;
    }
    if (r) cJSON_Delete(r);
  }
}

static void decide(const char* text) {
  EXT_RAM_BSS_ATTR static Job j;
  j.kind = JOB_DECIDE;
  snprintf(j.text, sizeof(j.text), "%s", text);
  utf8Trim(j.text);
  if (jobQ) xQueueSend(jobQ, &j, 0);
}

// ------------------------------------------------------------------ incoming messages
static char* msg = nullptr;  // reassembly buffer
static const int MSG_CAP = 256 * 1024;
static int msgLen = 0;
static uint8_t* pcmTmp = nullptr;
static const int PCM_CAP = 96 * 1024;

static int16_t* rsTmp = nullptr;  // resampled audio
EXT_RAM_BSS_ATTR static char rtTurn[1000];          // what the robot is saying in this Realtime response
static bool rtDrop = false;  // the person interrupted: ignore the rest of the cancelled answer
static char rtItem[48];        // the answer item being played and how much of it was queued (16 kHz)
static uint32_t rtItemSamples = 0;
static char truncMsg[160];     // conversation.item.truncate to send from the session loop
static bool liveDrop = false;  // GPT-Live: touched while talking, drop its audio until the person speaks

// stop what is playing (the server cancels / was told to); remember how much of the answer was heard
static void cutAnswer() {
  int queued = mic::streamQueued();
  mic::streamFlush();
  if (rt) {
    rtDrop = true;
    if (rtItem[0] && queued > 0) {
      uint32_t played = rtItemSamples > (uint32_t)queued ? rtItemSamples - queued : 0;
      snprintf(truncMsg, sizeof(truncMsg),
               "{\"type\":\"conversation.item.truncate\",\"item_id\":\"%s\",\"content_index\":0,"
               "\"audio_end_ms\":%u}", rtItem, (unsigned)(played / 16));
    }
    rtItem[0] = 0;
  } else {
    liveDrop = true;
  }
  xSemaphoreTake(txtMtx, portMAX_DELAY);
  if (outText[0]) pushTurn(false, outText);
  xSemaphoreGive(txtMtx);
}

static const char* failKind(int http, const char* code);

static void handleRealtime(char* m, int len) {
  const char* head = strstr(m, "\"type\"");
  if (head && head - m < 64 && !strncmp(head + 6, ":\"response.output_audio.delta\"", 30)) {
    const char* d = strstr(m, "\"delta\":\"");
    if (!d || rtDrop) return;
    const char* id = strstr(m, "\"item_id\":\"");
    if (id && id < d) {
      id += 11;
      const char* ie = strchr(id, '"');
      if (ie && ie - id < (int)sizeof(rtItem) && strncmp(rtItem, id, ie - id)) {
        snprintf(rtItem, sizeof(rtItem), "%.*s", (int)(ie - id), id);
        rtItemSamples = 0;
      }
    }
    d += 9;
    const char* e = strchr(d, '"');
    if (!e) return;
    int bytes = b64decode(d, e - d, pcmTmp, PCM_CAP);
    int n = downRs.run((const int16_t*)pcmTmp, bytes / 2, rsTmp);  // 24 -> 16 kHz
    mic::streamPush(rsTmp, n);
    rtItemSamples += n;
    return;
  }
  cJSON* j = cJSON_ParseWithLength(m, len);
  if (!j) return;
  const cJSON* t = cJSON_GetObjectItem(j, "type");
  const char* ty = cJSON_IsString(t) ? t->valuestring : "";
  int64_t now = esp_timer_get_time();
  if (!strcmp(ty, "session.updated")) {
    started = true;
  } else if (!strcmp(ty, "input_audio_buffer.speech_started")) {
    lastInUs = now;
    // the server cancels its answer, but a whole answer arrives in one burst: without dropping the
    // queued rest the old and the new answer both play ("Mach eine... Mach eine kleine Sache")
    cutAnswer();
  } else if (!strcmp(ty, "conversation.item.input_audio_transcription.completed")) {
    const cJSON* tr = cJSON_GetObjectItem(j, "transcript");
    if (cJSON_IsString(tr) && tr->valuestring[0]) {
      xSemaphoreTake(txtMtx, portMAX_DELAY);
      utf8Cat(inText, sizeof(inText), tr->valuestring);
      xSemaphoreGive(txtMtx);
      lastInUs = now;
      userSpokeFlag = true;
    }
  } else if (!strcmp(ty, "response.created")) {
    rtDrop = false;
    rtResponding = true;
    xSemaphoreTake(txtMtx, portMAX_DELAY);
    if (outText[0]) utf8Cat(outText, sizeof(outText), " ");  // a second answer in the same turn (after a tool)
    xSemaphoreGive(txtMtx);
    rtTurn[0] = 0;
    newTurn();
  } else if (!strcmp(ty, "response.output_audio_transcript.delta")) {
    const cJSON* d = cJSON_GetObjectItem(j, "delta");
    if (cJSON_IsString(d) && !rtDrop) {
      xSemaphoreTake(txtMtx, portMAX_DELAY);
      utf8Cat(outText, sizeof(outText), d->valuestring);
      xSemaphoreGive(txtMtx);
      utf8Cat(rtTurn, sizeof(rtTurn), d->valuestring);
      lastOutUs = now;
    }
  } else if (!strcmp(ty, "response.output_audio_transcript.done")) {
    if (rtTurn[0]) decide(rtTurn);  // the audio is still playing: the eyes react while it talks
  } else if (!strcmp(ty, "response.done")) {
    const cJSON* r = cJSON_GetObjectItem(j, "response");
    const cJSON* it;
    cJSON_ArrayForEach(it, cJSON_GetObjectItem(r, "output")) {
      const cJSON* it_t = cJSON_GetObjectItem(it, "type");
      if (!cJSON_IsString(it_t) || strcmp(it_t->valuestring, "function_call") || callW - callR >= 6) continue;
      const cJSON* id = cJSON_GetObjectItem(it, "call_id");
      const cJSON* nm = cJSON_GetObjectItem(it, "name");
      const cJSON* ar = cJSON_GetObjectItem(it, "arguments");
      if (!cJSON_IsString(id) || !cJSON_IsString(nm)) continue;
      ToolCall& c = calls[callW % 6];
      snprintf(c.id, sizeof(c.id), "%s", id->valuestring);
      snprintf(c.name, sizeof(c.name), "%s", nm->valuestring);
      c.args = nullptr;
      if (cJSON_IsString(ar)) {
        c.args = (char*)heap_caps_malloc(strlen(ar->valuestring) + 1, MALLOC_CAP_SPIRAM);
        strcpy(c.args, ar->valuestring);
      }
      callW = callW + 1;
      printf("tool: %s %s\n", c.name, c.args ? c.args : "");
    }
    const cJSON* u = cJSON_GetObjectItem(r, "usage");
    if (u) {  // what this answer cost (estimate)
      auto num = [](const cJSON* o, const char* k) {
        const cJSON* v = cJSON_GetObjectItem(o, k);
        return cJSON_IsNumber(v) ? v->valuedouble : 0.0;
      };
      const cJSON* in = cJSON_GetObjectItem(u, "input_token_details");
      const cJSON* cached = cJSON_GetObjectItem(in, "cached_tokens_details");
      const cJSON* out = cJSON_GetObjectItem(u, "output_token_details");
      double usd = ((num(in, "text_tokens") - num(cached, "text_tokens")) * CFG_PRICE_RT_TEXT_IN +
                    (num(in, "audio_tokens") - num(cached, "audio_tokens")) * CFG_PRICE_RT_AUDIO_IN +
                    num(in, "cached_tokens") * CFG_PRICE_RT_CACHED + num(out, "text_tokens") * CFG_PRICE_RT_TEXT_OUT +
                    num(out, "audio_tokens") * CFG_PRICE_RT_AUDIO_OUT) / 1e6;
      assist::addCost(strstr(cv.model, "mini") ? usd * 0.3 : usd);
      ESP_LOGI(TAG, "answer: in text %.0f (cached %.0f) audio %.0f, out text %.0f audio %.0f tokens -> %.4f USD",
               num(in, "text_tokens"), num(cached, "text_tokens"), num(in, "audio_tokens"), num(out, "text_tokens"),
               num(out, "audio_tokens"), usd);
    }
    const cJSON* sd_ = cJSON_GetObjectItem(cJSON_GetObjectItem(r, "status_details"), "error");
    const cJSON* code = cJSON_GetObjectItem(sd_, "code");
    if (cJSON_IsString(code)) {
      ESP_LOGW(TAG, "response failed: %s", code->valuestring);
      if (!strcmp(failKind(0, code->valuestring), "quota")) post(EV_ERROR, "quota");
    }
    rtResponding = false;
  } else if (!strcmp(ty, "error")) {
    const cJSON* er = cJSON_GetObjectItem(j, "error");
    const cJSON* c = cJSON_GetObjectItem(er, "code");
    const cJSON* ms = cJSON_GetObjectItem(er, "message");
    ESP_LOGW(TAG, "error %s: %.160s", cJSON_IsString(c) ? c->valuestring : "-", cJSON_IsString(ms) ? ms->valuestring : "");
    if (!started) snprintf(startError, sizeof(startError), "%s", cJSON_IsString(c) ? c->valuestring : "error");
    else if (cJSON_IsString(c) && !strcmp(failKind(0, c->valuestring), "quota"))
      post(EV_ERROR, "quota");
  }
  cJSON_Delete(j);
}

static void handleMessage(char* m, int len) {
  m[len] = 0;
  if (rt) {
    handleRealtime(m, len);
    return;
  }
  // fast path for the audio stream (most messages): no JSON tree for 40+ KB/s of base64
  const char* head = strstr(m, "\"type\"");
  if (head && head - m < 64 && !strncmp(head + 6, ":\"session.output_audio.delta\"", 29)) {
    const char* d = strstr(m, "\"delta\":\"");
    if (!d || liveDrop) return;
    d += 9;
    const char* e = strchr(d, '"');
    if (!e) return;
    int bytes = b64decode(d, e - d, pcmTmp, PCM_CAP);
    mic::streamPush((const int16_t*)pcmTmp, bytes / 2);
    return;
  }
  cJSON* j = cJSON_ParseWithLength(m, len);
  if (!j) return;
  const cJSON* t = cJSON_GetObjectItem(j, "type");
  const char* ty = cJSON_IsString(t) ? t->valuestring : "";
  int64_t now = esp_timer_get_time();
  if (!strcmp(ty, "session.input_transcript.delta")) {
    const cJSON* d = cJSON_GetObjectItem(j, "delta");
    if (cJSON_IsString(d)) {
      xSemaphoreTake(txtMtx, portMAX_DELAY);
      utf8Cat(inText, sizeof(inText), d->valuestring);
      xSemaphoreGive(txtMtx);
      lastInUs = now;
      userSpokeFlag = true;
      liveDrop = false;
    }
  } else if (!strcmp(ty, "session.output_transcript.delta")) {
    const cJSON* d = cJSON_GetObjectItem(j, "delta");
    if (cJSON_IsString(d) && !liveDrop) {
      if (now - lastOutUs > 1500000 || lastInUs > lastOutUs) newTurn();  // a new answer
      xSemaphoreTake(txtMtx, portMAX_DELAY);
      utf8Cat(outText, sizeof(outText), d->valuestring);
      xSemaphoreGive(txtMtx);
      lastOutUs = now;
    }
  } else if (!strcmp(ty, "decision.event")) {
    onDecision(j);
  } else if (!strcmp(ty, "response.event")) {
    onResponseEvent(cJSON_GetObjectItem(j, "event"));
  } else if (!strcmp(ty, "session.delegation.created")) {
    delegations = delegations + 1;
    post(EV_BUSY, "1");
    printf("backend: delegated\n");
  } else if (!strcmp(ty, "session.started")) {
    started = true;
  } else if (!strcmp(ty, "session.closed")) {
    const cJSON* r = cJSON_GetObjectItem(j, "reason");
    const cJSON* u = cJSON_GetObjectItem(cJSON_GetObjectItem(j, "usage"), "seconds");
    ESP_LOGI(TAG, "session closed (%s, %.0f s billed)", cJSON_IsString(r) ? r->valuestring : "?",
             cJSON_IsNumber(u) ? u->valuedouble : 0.0);
    if (cJSON_IsNumber(u)) assist::addCost(u->valuedouble / 60.0 * CFG_PRICE_LIVE_MIN);
    closedEv = true;
  } else if (!strcmp(ty, "error")) {
    const cJSON* er = cJSON_GetObjectItem(j, "error");
    const cJSON* c = cJSON_GetObjectItem(er, "code");
    const cJSON* ms = cJSON_GetObjectItem(er, "message");
    ESP_LOGW(TAG, "error %s: %.160s", cJSON_IsString(c) ? c->valuestring : "-", cJSON_IsString(ms) ? ms->valuestring : "");
    if (!started) snprintf(startError, sizeof(startError), "%s", cJSON_IsString(c) ? c->valuestring : "error");
    else if (cJSON_IsString(c) && !strcmp(failKind(0, c->valuestring), "quota"))
      post(EV_ERROR, "quota");
  }
  cJSON_Delete(j);
}

static void onWs(void*, esp_event_base_t, int32_t id, void* data) {
  auto* d = (esp_websocket_event_data_t*)data;
  switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
      handshakeStatus = 0;
      wsUp = true;
      break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
      wsUp = false;
      break;
    case WEBSOCKET_EVENT_ERROR:
      if (d->error_handle.esp_ws_handshake_status_code) handshakeStatus = d->error_handle.esp_ws_handshake_status_code;
      break;
    case WEBSOCKET_EVENT_DATA:
      if (d->op_code == 0x08) {  // close frame: the payload carries the reason
        ESP_LOGW(TAG, "closed by server (%d): %.*s", d->close_status_code, d->data_len > 2 ? d->data_len - 2 : 0,
                 d->data_ptr + 2);
        break;
      }
      if (d->op_code != 0x01 && d->op_code != 0x02 && d->op_code != 0x00) break;
      if (d->payload_offset == 0 && d->op_code != 0x00) msgLen = 0;
      if (msgLen + d->data_len < MSG_CAP - 1) {
        memcpy(msg + msgLen, d->data_ptr, d->data_len);
        msgLen += d->data_len;
      }
      if (d->payload_offset + d->data_len >= d->payload_len && d->fin) {
        handleMessage(msg, msgLen);
        msgLen = 0;
      }
      break;
    default:
      break;
  }
}

// ------------------------------------------------------------------ connection
static bool sendJson(const char* s, int timeoutMs = 3000) {
  return ws && wsUp && esp_websocket_client_send_text(ws, s, strlen(s), pdMS_TO_TICKS(timeoutMs)) >= 0;
}

static void dropSocket() {
  wsModel[0] = 0;
  if (ws) {
    esp_websocket_client_close(ws, pdMS_TO_TICKS(800));
    esp_websocket_client_destroy(ws);
    ws = nullptr;
  }
  wsUp = false;
}

// open the socket (TLS + upgrade) for a model. Blocks up to ~8 s.
static bool connectSocket(const char* model) {
  dropSocket();
  static char url[128];
  bool realtime = !strncmp(model, "gpt-realtime", 12);
  if (realtime)
    snprintf(url, sizeof(url), "wss://api.openai.com/v1/realtime?model=%s", model);
  else
    snprintf(url, sizeof(url), "%s", URL);
  static char headers[240];
  snprintf(headers, sizeof(headers), "Authorization: Bearer %s\r\n", settings::apiKey());
  esp_websocket_client_config_t cfg = {};
  cfg.uri = url;
  cfg.headers = headers;
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
  cfg.buffer_size = 8 * 1024;
  cfg.task_stack = 8 * 1024;
  cfg.network_timeout_ms = 8000;
  cfg.disable_auto_reconnect = true;
  cfg.ping_interval_sec = 15;  // keeps the idle socket (and NAT entries) alive, detects dead links
  cfg.pingpong_timeout_sec = 45;
  // core 0: Realtime sends a whole answer within ~2 s (decrypt, base64, 24 -> 16 kHz resampling), on
  // core 1 that halved the frame rate. The bands of a frame are shared by both cores dynamically, so
  // core 1 simply renders more of them while core 0 is busy here.
  cfg.task_prio = 5;
  cfg.task_core_id_set = true;
  cfg.task_core_id = 0;
  ws = esp_websocket_client_init(&cfg);
  if (!ws) return false;
  esp_websocket_register_events(ws, WEBSOCKET_EVENT_ANY, onWs, nullptr);
  wsUp = false;
  handshakeStatus = 0;
  if (esp_websocket_client_start(ws) != ESP_OK) return false;
  for (int i = 0; i < 170 && !wsUp && !handshakeStatus; ++i) vTaskDelay(pdMS_TO_TICKS(50));
  if (!wsUp) {
    ESP_LOGW(TAG, "connect failed (HTTP %d)", handshakeStatus);
    dropSocket();
  } else {
    snprintf(wsModel, sizeof(wsModel), "%s", model);
    wsOpenedUs = esp_timer_get_time();
    rtHasConv = false;
  }
  return wsUp;
}

// which error animation fits a failed start
static const char* failKind(int http, const char* code) {
  if (!net::connected()) return "offline";
  if (http == 401 || http == 403 || (code && (strstr(code, "api_key") || strstr(code, "auth")))) return "locked";
  if (http == 429 || (code && (strstr(code, "quota") || strstr(code, "rate_limit") || strstr(code, "billing") ||
                                    strstr(code, "credit"))))
    return "quota";
  if (http == 0 && !code) return "offline";  // no answer at all: no internet behind the WiFi
  return "glitch";
}

// ------------------------------------------------------------------ session setup
// when to end the conversation (a "nein" that answers a real question of the robot is no end)
#define END_WORDS                                                                                          \
  "z.B. stopp, Ruhe, sei still, nein, nein danke, nix, egal, schon gut, passt, das wars, tschuess - aber ein " \
  "nein als Antwort auf eine echte Frage von dir ist kein Ende"
// the newer tools, the same words for all three prompts (GPT-Live, its backend, Realtime)
#define TOOL_RULES                                                                                           \
  "- timer: Timer (Dauer in Sekunden umrechnen), Wecker und Erinnerungen stellen, auflisten, loeschen.\n"     \
  "- memory: sich etwas ueber den Nutzer merken oder es vergessen - nur wenn der Nutzer das will.\n"          \
  "- show_weather: Wettersymbol in den Augen, bei jedem Gespraech ueber das Wetter.\n"                       \
  "- system: Zustand, einschlafen, Nachtmodus. privacy (nicht mehr zuhoeren), restart, shutdown und "        \
  "clear_history NUR, wenn der Nutzer genau das ausdruecklich verlangt.\n"
static const char* MEMORY_HEAD = "\nDas hast du dir ueber den Nutzer gemerkt:\n";
static const char* LIVE_RULES =
    "\n\nBackchannel policy: Benutze keine Backchannels und keine Fuelllaute (kein mhm, aha, okay, ja ja), "
    "waehrend der Nutzer spricht. Bleib still, bis er fertig ist.\n"
    "Interruption policy: Wenn der Nutzer dich unterbricht, hoer sofort auf und hoer zu. Wiederhole nie, was du "
    "schon gesagt hast, und fang keinen Satz zweimal an.\n"
    "Wenn du etwas nicht kannst, sag es locker und jedes Mal anders (z.B. 'Da muss ich passen', 'Das krieg ich "
    "leider nicht hin', 'Dafuer fehlen mir die Haende') - nie steif wie 'das aendere ich hier nicht' oder 'kann ich "
    "hier nicht'.\n";

// OpenAI voices drift into an English accent on loanwords: pin language and pronunciation
static const char* SPEECH_RULES =
    "Language: Sprich Deutsch wie ein Muttersprachler aus Deutschland, mit natuerlicher deutscher Aussprache und "
    "Betonung - nie mit englischem Akzent. Speak German with a native German accent from Germany.\n"
    "Benutze keine englischen Woerter und Anglizismen (sag z.B. 'bereit' statt 'ready', 'Farbschema' statt "
    "'Theme', 'abgemacht' statt 'Deal', 'genau richtig' statt 'on point'). Nur Eigennamen und Produktnamen "
    "bleiben, wie sie sind. Zahlen, Uhrzeiten, Datumsangaben und Abkuerzungen sprichst du deutsch aus.\n";

static void buildInstructions(char* out, int cap, const char* homeList) {
  char facts[480];
  time_t now = time(nullptr);
  struct tm tmv;
  localtime_r(&now, &tmv);
  eyes::Look lk;
  settings::look(lk);
  snprintf(facts, sizeof(facts),
           "\nAktuell: %02d.%02d.%04d %02d:%02d Uhr. Deine IP-Adresse %s, WLAN-Signal %d dBm, Laufzeit %lld Minuten. "
           "Farbschema %s, Lautstaerke %d Prozent, Helligkeit %d Prozent, Stimme %s, Sprachmodell %s, "
           "Hintergrund-Modell %s.",
           tmv.tm_mday, tmv.tm_mon + 1, tmv.tm_year + 1900, tmv.tm_hour, tmv.tm_min, net::ip(), net::rssi(),
           esp_timer_get_time() / 60000000, lk.colorway >= 0 ? eyes::colorwayNameAt(lk.colorway) : "eigene Farben",
           cv.volume, settings::brightness(), cv.voice, cv.model, cv.backendModel);
  bool home = homeList && homeList[0];
  snprintf(out, cap,
           "%s%s%s"
           "Delegation policy:\n"
           "Backend tools:\n"
           "%s"
           "- Aussehen: deine Augenfarben, Farbschema, Augengroesse, Irisgroesse, Augenabstand, Glanzpunkte, "
           "Hintergrund, Displayhelligkeit, deine Lautstaerke und deine Stimme (z.B. maennlich oder weiblich) aendern.\n"
           "- Animationen: eine deiner Augen-Animationen vorfuehren, in eine Richtung schauen (links, rechts, oben, "
           "unten), dich bewegen (herumwandern, vom Rand hervorlugen, naeherkommen, huepfen, schaukeln), auch die "
           "Problem-Animationen (kein WLAN, Limit erreicht, kann ich nicht, nicht verstanden, kein Zugang, Stoerung).\n"
           TOOL_RULES
           "%s"
           "Delegate to the backend when:\n"
           "- Der Nutzer nach etwas Aktuellem fragt oder eine der Aktionen oben moechte.\n"
           "- Der Nutzer das Gespraech beenden will (" END_WORDS "). Sag dann gar nichts.\n"
           "- Eine Korrektur eine laufende Aufgabe aendert.\n"
           "Do not delegate to the backend when:\n"
           "- Du aus dem Gespraech oder deinem Wissen sicher antworten kannst (Smalltalk, Witze, Allgemeinwissen, "
           "Fragen ueber dich selbst).\n"
           "- Die Bitte keines der Backend-Werkzeuge oben braucht oder du sie gar nicht kannst%s. Sag dann direkt in "
           "einem kurzen Satz, dass das nicht geht.\n"
           "Delegate before giving an answer that depends on backend work. Do not guess the result while waiting.\n"
           "Bei Aktionen (Aussehen, Stimme, Animationen, Timer, Gedaechtnis%s) sprichst du nur ein einziges Mal: Warte schweigend auf das "
           "Ergebnis des Backends und sag dann einen ganz kurzen Satz. Sag vorher NICHTS - kein 'mach ich', 'klar', "
           "'okay' oder 'kann ich' und auch kein Summen oder andere Geraeusche. Nur bei Recherchen darfst du vorher "
           "kurz 'Moment' sagen.\n"
           "Wenn etwas nicht geht oder nicht klappt, sag das in einem kurzen, freundlichen Satz, ohne technische "
           "Details und ohne Fehlermeldungen.\n\n%s%s%s%s",
           cv.persona, LIVE_RULES, SPEECH_RULES,
           cv.webSearch ? "- Websuche: aktuelle Infos wie Wetter, Nachrichten, Preise, Oeffnungszeiten, Ergebnisse, "
                          "alles nach Juli 2025.\n"
                        : "",
           home ? "- Smart Home: Lampen schalten, dimmen, Farbe, Weisston und Effekte aendern, Schalter, Heizung, "
                  "Rolllaeden und mehr steuern und Zustaende abfragen.\n" : "",
           home ? "" : " (zum Beispiel Licht oder Geraete schalten: dafuer ist kein Smart Home eingerichtet)",
           home ? ", Smart Home" : "", cv.knowledge, assist::memory()[0] ? MEMORY_HEAD : "", assist::memory(), facts);
}

static void buildBackendInstructions(char* out, int cap, const char* homeList) {
  time_t now = time(nullptr);
  struct tm tmv;
  localtime_r(&now, &tmv);
  bool home = homeList && homeList[0];
  snprintf(out, cap,
           "Du bist das Backend von %s, einem kleinen sprechenden Roboter mit animierten Augen. Der Sprach-Teil redet "
           "mit dem Nutzer und gibt dir Aufgaben; die Transkripte koennen Hoerfehler enthalten.\n"
           "- Recherche: nutze die Websuche und antworte mit den wichtigsten Fakten in ein bis zwei kurzen Saetzen, so "
           "wie man sie spricht (Zahlen und Einheiten ausgeschrieben, keine Listen, kein Markdown, keine Links).\n"
           "- Aussehen oder Lautstaerke: rufe change_look mit genau den Feldern auf, die sich aendern sollen. Farben als "
           "Hex-Wert. Fertige Farbschemata: porcelain, gumball, grape soda, marmalade, mint, bubblegum, lemon, ocean, "
           "classic, lime.\n"
           "- Animationen vorfuehren: play_animation (auch die Problem-Animationen, z.B. glitch fuer 'Verbindung zur KI "
           "gestoert', offline fuer 'kein WLAN'). Eine vorgefuehrte Problem-Animation ist nur eine Demo: melde danach, "
           "dass du sie zeigst, nie dass etwas nicht klappt.\n"
           "%s"
           "Du hast NUR diese Werkzeuge. Behaupte nie, etwas getan zu haben, ohne das passende Werkzeug aufgerufen zu "
           "haben. Kannst du eine Bitte damit nicht erfuellen, antworte nur: Das kann ich leider nicht.\n"
           "Stell keine Rueckfragen - nimm die naheliegendste Bedeutung und handle.\n"
           "- Gespraech beenden: rufe end_conversation auf, wenn der Nutzer nichts mehr will (" END_WORDS ").\n"
           TOOL_RULES
           "Melde danach in hoechstens einem kurzen Satz, was passiert ist. Wenn etwas nicht klappt, sag knapp, dass "
           "es nicht ging.\n"
           "Jetzt ist der %02d.%02d.%04d, %02d:%02d Uhr (Deutschland).%s%s",
           cv.name,
           home ? "- Smart Home: Lampen mit set_light, alles andere mit smart_home, immer mit der exakten entity_id "
                  "aus der Geraeteliste (Format entity_id|Name|Zustand, bei Lampen dazu |Farbmodi|Weisston-Bereich|"
                  "Effekte). Farbe nur bei Farbmodus hs, rgb, rgbw, rgbww oder xy, Weisston nur bei color_temp. Farben "
                  "als rgb_color. Fuer Fragen nach Zustaenden reicht oft die Liste.\n"
                : "",
           tmv.tm_mday, tmv.tm_mon + 1, tmv.tm_year + 1900, tmv.tm_hour, tmv.tm_min, home ? "\nGeraete:\n" : "",
           home ? homeList : "");
}

// a list of changes (with one object of optional fields the small model filled in everything)
static const char* CHANGE_LOOK =
    "{\"type\":\"function\",\"name\":\"change_look\",\"description\":\"Aendert Pixels Aussehen oder Lautstaerke. "
    "Liste NUR die Einstellungen auf, die der Nutzer aendern will - alles andere bleibt wie es ist.\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{\"changes\":{\"type\":\"array\",\"items\":{"
    "\"type\":\"object\",\"properties\":{\"setting\":{\"type\":\"string\",\"enum\":[\"colorway\","
    "\"sclera_color\",\"iris_color\",\"shine_color\",\"background_color\",\"eye_size\",\"iris_size\","
    "\"eye_spacing\",\"highlights\",\"brightness\",\"volume\",\"voice\",\"reset\"]},\"value\":{\"type\":\"string\","
    "\"description\":\"colorway: porcelain, gumball, grape soda, marmalade, mint, bubblegum, lemon, ocean, classic "
    "oder lime. Farben (sclera=Augenweiss, iris, shine=Glanzpunkte, background) als Hex wie #ff0000. eye_size 0.6-1.3, "
    "iris_size 0.5-1.5, eye_spacing 0.75-1.25 (normal 1), highlights 0-1, brightness 5-100, volume 0-100, voice: "
    "marin coral sage shimmer (weiblich), cedar ash echo verse ballad (maennlich), alloy. reset: true.\"}},\"required\":[\"setting\",\"value\"]}}},"
    "\"required\":[\"changes\"]}}";

static const char* SMART_HOME =
    "{\"type\":\"function\",\"name\":\"smart_home\",\"description\":\"Steuert ein Geraet im Smart Home oder fragt "
    "seinen Zustand ab.\",\"parameters\":{\"type\":\"object\",\"properties\":{"
    "\"entity_id\":{\"type\":\"string\",\"description\":\"exakte entity_id aus der Geraeteliste\"},"
    "\"action\":{\"type\":\"string\",\"enum\":[\"turn_on\",\"turn_off\",\"toggle\",\"set_brightness\","
    "\"set_temperature\",\"set_position\",\"open\",\"close\",\"get_state\"]},"
    "\"value\":{\"type\":\"number\",\"description\":\"Helligkeit 0-100, Temperatur in Grad oder Position 0-100\"}},"
    "\"required\":[\"entity_id\",\"action\"]}}";

static const char* END_CONVERSATION =
    "{\"type\":\"function\",\"name\":\"end_conversation\",\"description\":\"Beendet das Gespraech sofort und "
    "still, wenn der Nutzer nichts mehr will oder Ruhe moechte.\",\"parameters\":{\"type\":\"object\","
    "\"properties\":{}}}";

static const char* TIMER_FN =
    "{\"type\":\"function\",\"name\":\"timer\",\"description\":\"Timer, Wecker und Erinnerungen (klingeln "
    "hier am Roboter).\",\"parameters\":{\"type\":\"object\",\"properties\":{"
    "\"action\":{\"type\":\"string\",\"enum\":[\"set\",\"alarm\",\"cancel\",\"list\"],\"description\":"
    "\"set = Timer in seconds Sekunden, alarm = Wecker um hour:minute (naechstes Mal), cancel = loeschen (label oder "
    "alle), list = was ist gestellt\"},\"seconds\":{\"type\":\"integer\"},\"hour\":{\"type\":\"integer\"},"
    "\"minute\":{\"type\":\"integer\"},\"label\":{\"type\":\"string\",\"description\":\"wofuer, z.B. Nudeln "
    "oder Aufstehen\"}},\"required\":[\"action\"]}}";

static const char* MEMORY_FN =
    "{\"type\":\"function\",\"name\":\"memory\",\"description\":\"Merkt sich etwas ueber den Nutzer dauerhaft "
    "(Name, Vorlieben, Wichtiges) oder vergisst es wieder. Nur wenn der Nutzer das will (merk dir, vergiss).\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"enum\":"
    "[\"remember\",\"forget\"]},\"text\":{\"type\":\"string\",\"description\":\"remember: der Fakt als kurzer "
    "Satz. forget: ein Stichwort daraus, oder alles\"}},\"required\":[\"action\",\"text\"]}}";

static const char* WEATHER_FN =
    "{\"type\":\"function\",\"name\":\"show_weather\",\"description\":\"Zeigt ein Wettersymbol in deinen "
    "Augen. Immer aufrufen, wenn du ueber das Wetter sprichst.\",\"parameters\":{\"type\":\"object\","
    "\"properties\":{\"condition\":{\"type\":\"string\",\"enum\":[\"sunny\",\"partlycloudy\",\"cloudy\","
    "\"rainy\",\"snowy\",\"lightning\",\"fog\",\"clear-night\"]}},\"required\":[\"condition\"]}}";

// self management; the sensitive actions are also checked against what the person really said
static const char* SYSTEM_FN =
    "{\"type\":\"function\",\"name\":\"system\",\"description\":\"Selbstverwaltung. status = Zustand (WLAN, "
    "Laufzeit, Speicher, Kosten, Timer); sleep = einschlafen; night_on / night_off = Nachtmodus. NUR wenn der Nutzer es "
    "ausdruecklich verlangt: privacy = nicht mehr zuhoeren bis zur naechsten Beruehrung, restart = neu starten, "
    "shutdown = ausschalten, clear_history = Gespraechsverlauf loeschen, update_install = die neue Version von GitHub "
    "installieren (er startet danach neu). update_check = nach einer neuen Version suchen (jederzeit).\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"enum\":[\"status\","
    "\"sleep\",\"night_on\",\"night_off\",\"privacy\",\"restart\",\"shutdown\",\"clear_history\",\"update_check\","
    "\"update_install\"]}},\"required\":[\"action\"]}}";

static const char* SET_LIGHT =
    "{\"type\":\"function\",\"name\":\"set_light\",\"description\":\"Schaltet eine Lampe ein oder aus und "
    "aendert Helligkeit, Farbe, Weisston oder Effekt. Farbe und Weisston nie gleichzeitig.\",\"parameters\":{"
    "\"type\":\"object\",\"properties\":{"
    "\"entity_id\":{\"type\":\"string\",\"description\":\"exakte light.* entity_id aus der Geraeteliste\"},"
    "\"on\":{\"type\":\"boolean\",\"description\":\"false = ausschalten (alles andere wird ignoriert)\"},"
    "\"rgb_color\":{\"type\":\"array\",\"items\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":255},"
    "\"minItems\":3,\"maxItems\":3,\"description\":\"Farbe [r,g,b]\"},"
    "\"brightness_pct\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":100},"
    "\"brightness_step_pct\":{\"type\":\"integer\",\"minimum\":-100,\"maximum\":100,\"description\":"
    "\"relativ heller (+) oder dunkler (-)\"},"
    "\"color_temp_kelvin\":{\"type\":\"integer\",\"minimum\":2000,\"maximum\":6500,\"description\":"
    "\"Weisston: 2700 warmweiss, 4000 neutral, 6500 kaltweiss\"},"
    "\"effect\":{\"type\":\"string\",\"description\":\"Effekt aus der Geraeteliste\"}},"
    "\"required\":[\"entity_id\",\"on\"]}}";

static const char* PLAY_ANIMATION =
    "{\"type\":\"function\",\"name\":\"play_animation\",\"description\":\"Fuehrt eine von Pixels "
    "Augen-Animationen vor, wenn der Nutzer das moechte.\",\"parameters\":{\"type\":\"object\",\"properties\":{"
    "\"name\":{\"type\":\"string\",\"enum\":[\"yes\",\"no\",\"dont_know\",\"happy\",\"laugh\","
    "\"love\",\"excited\",\"proud\",\"sad\",\"cry\",\"surprised\",\"scared\",\"angry\",\"confused\","
    "\"shy\",\"wink\",\"hello\",\"sleep\",\"dizzy\",\"bored\",\"suspicious\",\"curious\",\"dance\","
    "\"offline\",\"quota\",\"cant\",\"huh\",\"locked\",\"glitch\",\"light_on\",\"light_off\",\"light_color\","
    "\"wander\",\"peek\",\"come_closer\",\"bounce\",\"squish\",\"sway\",\"look_left\",\"look_right\",\"look_up\","
    "\"look_down\",\"battery\"],\"description\":\"battery=zeigt den echten Akkustand, look_*=fuer ein paar Sekunden in diese Richtung schauen, wander=woanders "
    "hinwandern, peek=vom Rand hervorlugen, come_closer=naeherkommen, bounce=huepfen, squish=Augen quetschen, "
    "sway=hin und her schaukeln, excited=Sternaugen, light_*=Gluehbirnen-Augen (Licht an/aus/Farbwechsel), "
    "love=Herzaugen, bored=Augen rollen. Probleme: offline=kein WLAN, quota=Nutzungslimit erreicht (Tacho im roten Bereich), "
    "cant=kann ich nicht, huh=nicht verstanden, locked=kein Zugang/Schluessel falsch, glitch=Stoerung/Server oder "
    "KI nicht erreichbar\"}},\"required\":[\"name\"]}}";

// play_animation with the own animations from the web interface added to its list
static cJSON* playAnimationTool() {
  cJSON* t = cJSON_Parse(PLAY_ANIMATION);
  int n = settings::customCount();
  if (!n) return t;
  cJSON* name = cJSON_GetObjectItem(cJSON_GetObjectItem(cJSON_GetObjectItem(t, "parameters"), "properties"), "name");
  cJSON* en = cJSON_GetObjectItem(name, "enum");
  char d[1200];
  const cJSON* od = cJSON_GetObjectItem(name, "description");
  int k = snprintf(d, sizeof(d), "%s. Eigene Animationen des Nutzers (Ablaeufe, ruhig mit dem Namen anbieten):",
                   cJSON_IsString(od) ? od->valuestring : "");
  for (int i = 0; i < n; ++i) {
    cJSON_AddItemToArray(en, cJSON_CreateString(settings::customName(i)));
    if (k < (int)sizeof(d)) k += snprintf(d + k, sizeof(d) - k, "%s \"%s\"", i ? "," : "", settings::customName(i));
  }
  cJSON_ReplaceItemInObject(name, "description", cJSON_CreateString(d));
  return t;
}

// eye animations: picked while the robot talks (and whether it says it can't / didn't understand)
static const char* TRIGGER_OUT =
    "{\"type\":\"decisions\",\"event_type\":\"session.output_transcript.delta\",\"decisions\":{\"questions\":["
    "{\"type\":\"choice\",\"name\":\"anim\",\"instructions\":\"Pixel ist ein kleiner Roboter mit animierten Augen. "
    "Welche Augen-Animation passt zu dem, was Pixel gerade sagt? none, wenn nichts Besonderes passt.\","
    "\"choices\":[{\"value\":\"none\"},{\"value\":\"yes\",\"description\":\"zustimmen, ja\"},"
    "{\"value\":\"no\",\"description\":\"verneinen, nein\"},{\"value\":\"dont_know\",\"description\":\"weiss es "
    "nicht, Ansichtssache\"},{\"value\":\"happy\",\"description\":\"freut sich\"},{\"value\":\"laugh\","
    "\"description\":\"Witz, lustig\"},{\"value\":\"love\",\"description\":\"Kompliment, Zuneigung\"},"
    "{\"value\":\"excited\",\"description\":\"begeistert, wow\"},{\"value\":\"proud\",\"description\":\"stolz, "
    "gelobt\"},{\"value\":\"sad\",\"description\":\"traurig, Mitgefuehl\"},{\"value\":\"surprised\","
    "\"description\":\"ueberrascht\"},{\"value\":\"scared\",\"description\":\"Angst, gruselig\"},"
    "{\"value\":\"angry\",\"description\":\"genervt, beleidigt\"},{\"value\":\"curious\",\"description\":"
    "\"neugierig, nachdenklich\"},{\"value\":\"shy\",\"description\":\"verlegen\"},{\"value\":\"wink\","
    "\"description\":\"verschmitzt, Tschuess\"},{\"value\":\"hello\",\"description\":\"Begruessung\"},"
    "{\"value\":\"sleep\",\"description\":\"gute Nacht\"},{\"value\":\"dance\",\"description\":\"Musik, "
    "tanzen\"},{\"value\":\"bored\",\"description\":\"langweilig, Augenrollen\"},{\"value\":\"suspicious\","
    "\"description\":\"misstrauisch\"}]},"
    "{\"type\":\"choice\",\"name\":\"situation\",\"instructions\":\"Was trifft auf das zu, was Pixel gerade "
    "sagt?\",\"choices\":[{\"value\":\"normal\"},{\"value\":\"cannot\",\"description\":\"Pixel sagt, dass es "
    "etwas nicht kann, nicht darf oder dass etwas nicht geklappt hat\"},{\"value\":\"not_understood\","
    "\"description\":\"Pixel hat den Nutzer akustisch nicht verstanden und bittet um Wiederholung\"}]}]}}";

// listening mood: picked while the person talks
static const char* TRIGGER_IN =
    "{\"type\":\"decisions\",\"event_type\":\"session.input_transcript.delta\",\"decisions\":{\"questions\":["
    "{\"type\":\"choice\",\"name\":\"mood\",\"instructions\":\"Pixel ist ein kleiner Roboter mit animierten Augen und "
    "hoert gerade zu. Wie sollte sein Gesicht auf das reagieren, was der Nutzer gerade sagt?\",\"choices\":["
    "{\"value\":\"none\"},{\"value\":\"happy\",\"description\":\"gute Nachricht, Freude\"},{\"value\":\"sad\","
    "\"description\":\"traurige Nachricht\"},{\"value\":\"surprised\",\"description\":\"ueberraschend\"},"
    "{\"value\":\"love\",\"description\":\"Kompliment an Pixel, Zuneigung\"},{\"value\":\"curious\","
    "\"description\":\"Frage, Raetsel\"},{\"value\":\"funny\",\"description\":\"lustig, Witz\"},"
    "{\"value\":\"worried\",\"description\":\"Sorge, Angst, Problem\"}]}]}}";

static char* homeDevices = nullptr;  // Home Assistant device list (refreshed between sessions)
static const int HOME_CAP = 16384;  // a home with a few hundred lamps, switches and sensors

static const char* WEB_SEARCH_FN =
    "{\"type\":\"function\",\"name\":\"web_search\",\"description\":\"Sucht im Internet nach aktuellen Infos "
    "(Wetter, Nachrichten, Preise, Oeffnungszeiten, Ergebnisse, alles nach 2024).\",\"parameters\":{\"type\":"
    "\"object\",\"properties\":{\"query\":{\"type\":\"string\",\"description\":\"Suchanfrage auf "
    "Deutsch, mit Ort und Datum falls wichtig\"}},\"required\":[\"query\"]}}";

// voices of the Realtime API (GPT-Live has more)
static const char* rtVoice(const char* v) {
  for (const char* k : {"alloy", "ash", "ballad", "coral", "echo", "sage", "shimmer", "verse", "marin", "cedar"})
    if (!strcmp(k, v)) return v;
  return "marin";
}

// Realtime keeps its session (socket) open between conversations within resume_s: an idle Realtime
// session costs nothing, and its instructions then come from the cache (about half the price per
// answer). So the instructions hold only what rarely changes; clock and device states go into a
// short context message at the start of every conversation.
static char* rtIns = nullptr;    // the instructions this session got (PSRAM)

static void sendText(const char* role, const char* type, const char* text) {
  cJSON* e = cJSON_CreateObject();
  cJSON_AddStringToObject(e, "type", "conversation.item.create");
  cJSON* it = cJSON_AddObjectToObject(e, "item");
  cJSON_AddStringToObject(it, "type", "message");
  cJSON_AddStringToObject(it, "role", role);
  cJSON* part = cJSON_CreateObject();
  cJSON_AddStringToObject(part, "type", type);
  cJSON_AddStringToObject(part, "text", text);
  cJSON_AddItemToArray(cJSON_AddArrayToObject(it, "content"), part);
  char* t = cJSON_PrintUnformatted(e);
  cJSON_Delete(e);
  sendJson(t);
  cJSON_free(t);
}

static bool sendStartRealtime(bool resume) {
  const int ICAP = 32768;  // persona + knowledge + device list
  char* ins = (char*)heap_caps_malloc(ICAP, MALLOC_CAP_SPIRAM);
  bool home = ha::configured(cv) && homeDevices && homeDevices[0];
  char facts[480];
  time_t now = time(nullptr);
  struct tm tmv;
  localtime_r(&now, &tmv);
  snprintf(facts, sizeof(facts), "\nAktuell: %02d.%02d.%04d %02d:%02d Uhr. Deine IP-Adresse %s, Lautstaerke %d Prozent, "
           "Stimme %s, Sprachmodell %s.", tmv.tm_mday, tmv.tm_mon + 1, tmv.tm_year + 1900, tmv.tm_hour, tmv.tm_min,
           net::ip(), cv.volume, rtVoice(cv.voice), cv.model);
  snprintf(ins, ICAP,
           "%s\n%s"
           "Werkzeuge: %schange_look fuer dein Aussehen, deine Stimme und Lautstaerke; play_animation um eine deiner "
           "Augen-Animationen vorzufuehren, in eine Richtung zu schauen oder dich zu bewegen (wandern, huepfen, "
           "naeherkommen ...)%s. Behaupte nie, etwas getan zu haben, ohne "
           "das Werkzeug benutzt zu haben, und erfinde keine aktuellen Fakten.\n"
           "Bei Aktionen sprichst du nur einmal: erst das Werkzeug, dann EIN ganz kurzer Satz. Nur vor einer Websuche "
           "darfst du kurz 'Moment' sagen. Wenn du etwas nicht kannst, sag es locker und jedes Mal anders (z.B. 'Da "
           "muss ich passen', 'Das krieg ich leider nicht hin') - ohne technische Details oder Fehlermeldungen. "
           "Keine Geraeusche wie Summen. Never end your reply with a question.\n"
           "Will der Nutzer nichts mehr (" END_WORDS "), rufe nur end_conversation auf und sag NICHTS.\n"
           "Weitere Werkzeuge:\n" TOOL_RULES "\n%s%s%s%s%s%s",
           cv.persona, SPEECH_RULES, cv.webSearch ? "web_search fuer aktuelle Infos; " : "",
           home ? "; set_light fuer Lampen (an/aus, Helligkeit, Farbe als rgb_color, Weisston, Effekt) und "
                  "smart_home fuer alle anderen Geraete, immer mit der exakten entity_id aus der Liste unten. Farbe "
                  "nur bei Farbmodus hs, rgb, rgbw, rgbww oder xy, Weisston nur bei color_temp" : "",
           // fixed parts first: the same beginning as last time is billed as cached input (10x cheaper)
           cv.knowledge, assist::memory()[0] ? MEMORY_HEAD : "", assist::memory(), "", "", "");

  // the context of this conversation: clock, own state, the devices as they are now
  const int CTX_CAP = HOME_CAP + 1024;
  char* ctx = (char*)heap_caps_malloc(CTX_CAP, MALLOC_CAP_SPIRAM);
  snprintf(ctx, CTX_CAP, "(Kontext, nicht vorlesen)%s%s%s", facts,
           home ? "\nGeraete (entity_id|Name|Zustand, bei Lampen dazu |Farbmodi|Weisston-Bereich|Effekte):\n" : "",
           home ? homeDevices : "");
  if (!rtIns) rtIns = (char*)heap_caps_calloc(1, ICAP, MALLOC_CAP_SPIRAM);
  bool same = rtHasConv && !strcmp(ins, rtIns);
  bool ok = true;
  if (same) {  // the session is still configured: no session.update, the cache stays valid
    free(ins);
    started = true;
  } else {
  snprintf(rtIns, ICAP, "%s", ins);
  snprintf(rtSessVoice, sizeof(rtSessVoice), "%s", rtVoice(cv.voice));
  cJSON* root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "type", "session.update");
  cJSON* s = cJSON_AddObjectToObject(root, "session");
  cJSON_AddStringToObject(s, "type", "realtime");
  cJSON_AddStringToObject(s, "instructions", ins);
  free(ins);
  cJSON_AddItemToArray(cJSON_AddArrayToObject(s, "output_modalities"), cJSON_CreateString("audio"));
  cJSON* au = cJSON_AddObjectToObject(s, "audio");
  cJSON* in = cJSON_AddObjectToObject(au, "input");
  cJSON* f1 = cJSON_AddObjectToObject(in, "format");
  cJSON_AddStringToObject(f1, "type", "audio/pcm");
  cJSON_AddNumberToObject(f1, "rate", 24000);
  cJSON* tr = cJSON_AddObjectToObject(in, "transcription");
  cJSON_AddStringToObject(tr, "model", "gpt-4o-mini-transcribe");
  cJSON_AddStringToObject(tr, "language", "de");
  cJSON* td = cJSON_AddObjectToObject(in, "turn_detection");
  cJSON_AddStringToObject(td, "type", "server_vad");
  cJSON_AddNumberToObject(td, "silence_duration_ms", 500);
  cJSON_AddNumberToObject(td, "threshold", 0.4);  // the room microphone is quiet
  cJSON_AddNumberToObject(td, "prefix_padding_ms", 300);
  cJSON_AddStringToObject(cJSON_AddObjectToObject(in, "noise_reduction"), "type", "far_field");
  cJSON* out = cJSON_AddObjectToObject(au, "output");
  cJSON* f2 = cJSON_AddObjectToObject(out, "format");
  cJSON_AddStringToObject(f2, "type", "audio/pcm");
  cJSON_AddNumberToObject(f2, "rate", 24000);
  cJSON_AddStringToObject(out, "voice", rtVoice(cv.voice));
  cJSON* tools = cJSON_AddArrayToObject(s, "tools");
  cJSON_AddItemToArray(tools, cJSON_Parse(CHANGE_LOOK));
  cJSON_AddItemToArray(tools, cJSON_Parse(END_CONVERSATION));
  for (const char* f : {TIMER_FN, MEMORY_FN, WEATHER_FN, SYSTEM_FN}) cJSON_AddItemToArray(tools, cJSON_Parse(f));
  cJSON_AddItemToArray(tools, playAnimationTool());
  if (cv.webSearch) cJSON_AddItemToArray(tools, cJSON_Parse(WEB_SEARCH_FN));
  if (home) cJSON_AddItemToArray(tools, cJSON_Parse(SMART_HOME));
  if (home) cJSON_AddItemToArray(tools, cJSON_Parse(SET_LIGHT));
  cJSON_AddStringToObject(s, "tool_choice", "auto");
  cJSON_AddStringToObject(cJSON_AddObjectToObject(s, "reasoning"), "effort", "low");
  char* txt = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  ok = txt && sendJson(txt);
  ESP_LOGI(TAG, "realtime session.update (%d bytes, %s, voice %s)", txt ? (int)strlen(txt) : 0,
           resume ? "with history" : "new", rtVoice(cv.voice));
  cJSON_free(txt);
  }
  // the conversation so far (unless this session still has it)
  for (int i = 0; ok && resume && !rtHasConv && i < histN; ++i) {
    cJSON* e = cJSON_CreateObject();
    cJSON_AddStringToObject(e, "type", "conversation.item.create");
    cJSON* it = cJSON_AddObjectToObject(e, "item");
    cJSON_AddStringToObject(it, "type", "message");
    cJSON_AddStringToObject(it, "role", hist[i].user ? "user" : "assistant");
    cJSON* part = cJSON_CreateObject();
    cJSON_AddStringToObject(part, "type", hist[i].user ? "input_text" : "output_text");
    cJSON_AddStringToObject(part, "text", hist[i].text);
    cJSON_AddItemToArray(cJSON_AddArrayToObject(it, "content"), part);
    char* t = cJSON_PrintUnformatted(e);
    cJSON_Delete(e);
    ok = sendJson(t);
    cJSON_free(t);
  }
  if (ok) {
    sendJson("{\"type\":\"input_audio_buffer.clear\"}");
    sendText("system", "input_text", ctx);
    rtHasConv = true;
  }
  free(ctx);
  return ok;
}

static bool sendStart(bool resume) {
  if (rt) return sendStartRealtime(resume);
  const int ICAP = 32768;  // persona + knowledge + device list
  char* ins = (char*)heap_caps_malloc(ICAP, MALLOC_CAP_SPIRAM);
  char* bins = (char*)heap_caps_malloc(ICAP, MALLOC_CAP_SPIRAM);
  bool home = ha::configured(cv) && homeDevices && homeDevices[0];
  buildInstructions(ins, ICAP, home ? homeDevices : nullptr);
  buildBackendInstructions(bins, ICAP, home ? homeDevices : nullptr);

  cJSON* root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "type", "session.start");
  cJSON_AddStringToObject(root, "event_id", "start");
  cJSON* s = cJSON_AddObjectToObject(root, "session");
  cJSON_AddStringToObject(s, "model", cv.model);
  cJSON_AddStringToObject(s, "instructions", ins);
  cJSON* au = cJSON_AddObjectToObject(s, "audio");
  cJSON* fm = cJSON_AddObjectToObject(au, "format");
  cJSON_AddStringToObject(fm, "type", "audio/pcm");
  cJSON_AddNumberToObject(fm, "rate", 16000);
  cJSON_AddStringToObject(cJSON_AddObjectToObject(au, "output"), "voice", cv.voice);
  if (resume && histN) {  // the conversation so far
    cJSON* in = cJSON_AddArrayToObject(s, "input");
    for (int i = 0; i < histN; ++i) {
      cJSON* m = cJSON_CreateObject();
      cJSON_AddStringToObject(m, "type", "message");
      cJSON_AddStringToObject(m, "role", hist[i].user ? "user" : "assistant");
      cJSON* part = cJSON_CreateObject();
      cJSON_AddStringToObject(part, "type", hist[i].user ? "input_text" : "output_text");
      cJSON_AddStringToObject(part, "text", hist[i].text);
      cJSON_AddItemToArray(cJSON_AddArrayToObject(m, "content"), part);
      cJSON_AddItemToArray(in, m);
    }
  }
  cJSON* dl = cJSON_AddObjectToObject(s, "delegation");
  cJSON_AddStringToObject(dl, "type", "responses");
  cJSON* rs = cJSON_AddObjectToObject(dl, "responses");
  cJSON_AddStringToObject(rs, "model", cv.backendModel);
  cJSON_AddStringToObject(rs, "instructions", bins);
  cJSON* tools = cJSON_AddArrayToObject(rs, "tools");
  if (cv.webSearch) {
    cJSON* w = cJSON_CreateObject();
    cJSON_AddStringToObject(w, "type", "web_search");
    cJSON_AddItemToArray(tools, w);
  }
  cJSON_AddItemToArray(tools, cJSON_Parse(CHANGE_LOOK));
  cJSON_AddItemToArray(tools, cJSON_Parse(END_CONVERSATION));
  for (const char* f : {TIMER_FN, MEMORY_FN, WEATHER_FN, SYSTEM_FN}) cJSON_AddItemToArray(tools, cJSON_Parse(f));
  cJSON_AddItemToArray(tools, playAnimationTool());
  if (home) cJSON_AddItemToArray(tools, cJSON_Parse(SMART_HOME));
  if (home) cJSON_AddItemToArray(tools, cJSON_Parse(SET_LIGHT));
  cJSON_AddStringToObject(rs, "tool_choice", "auto");
  if (cv.backendEffort[0]) cJSON_AddStringToObject(cJSON_AddObjectToObject(rs, "reasoning"), "effort", cv.backendEffort);
  cJSON* tr = cJSON_AddArrayToObject(s, "triggers");
  cJSON_AddItemToArray(tr, cJSON_Parse(TRIGGER_OUT));
  cJSON_AddItemToArray(tr, cJSON_Parse(TRIGGER_IN));
  free(ins);
  free(bins);
  char* txt = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  bool ok = txt && sendJson(txt);
  ESP_LOGI(TAG, "session.start (%d bytes, %s, voice %s, backend %s)", txt ? (int)strlen(txt) : 0,
           resume ? "with history" : "new", cv.voice, cv.backendModel);
  cJSON_free(txt);
  return ok;
}

// ------------------------------------------------------------------ tools
static void colorField(cJSON* look, const char* key, const cJSON* v) {
  if (cJSON_IsString(v) && v->valuestring[0] == '#' && strlen(v->valuestring) == 7)
    cJSON_AddStringToObject(look, key, v->valuestring);
}

static bool changeLook(const cJSON* a, char* result, int cap) {
  cJSON* patch = cJSON_CreateObject();
  cJSON* look = cJSON_AddObjectToObject(patch, "look");
  eyes::Look cur;
  settings::look(cur);
  if (cJSON_IsTrue(cJSON_GetObjectItem(a, "reset"))) {
    eyes::Look d;
    cJSON_AddNumberToObject(look, "colorway", 0);
    cJSON_AddNumberToObject(look, "eye_w", d.eyeW);
    cJSON_AddNumberToObject(look, "eye_h", d.eyeH);
    cJSON_AddNumberToObject(look, "iris_w", d.irisW);
    cJSON_AddNumberToObject(look, "iris_h", d.irisH);
    cJSON_AddNumberToObject(look, "spacing", d.spacing);
    cJSON_AddNumberToObject(look, "highlights", d.highlights);
    cJSON_AddStringToObject(look, "bg", "#000000");
  }
  const cJSON* cw = cJSON_GetObjectItem(a, "colorway");
  if (cJSON_IsString(cw))
    for (int i = 0; i < eyes::colorwayCount(); ++i)
      if (!strcasecmp(cw->valuestring, eyes::colorwayNameAt(i))) cJSON_AddNumberToObject(look, "colorway", i);
  const cJSON* sc = cJSON_GetObjectItem(a, "sclera_color");
  const cJSON* ic = cJSON_GetObjectItem(a, "iris_color");
  const cJSON* hc = cJSON_GetObjectItem(a, "shine_color");
  if (sc || ic || hc) {
    if (cur.colorway >= 0) {  // start the custom colours from the current preset
      char b[8];
      snprintf(b, sizeof(b), "#%06lX", (unsigned long)eyes::colorwayColor(cur.colorway, 0));
      cJSON_AddStringToObject(look, "sclera", b);
      snprintf(b, sizeof(b), "#%06lX", (unsigned long)eyes::colorwayColor(cur.colorway, 1));
      cJSON_AddStringToObject(look, "iris", b);
      snprintf(b, sizeof(b), "#%06lX", (unsigned long)eyes::colorwayColor(cur.colorway, 2));
      cJSON_AddStringToObject(look, "accent", b);
    }
    cJSON_DeleteItemFromObject(look, "colorway");
    cJSON_AddNumberToObject(look, "colorway", -1);
    if (sc) {
      cJSON_DeleteItemFromObject(look, "sclera");
      colorField(look, "sclera", sc);
    }
    if (ic) {
      cJSON_DeleteItemFromObject(look, "iris");
      colorField(look, "iris", ic);
    }
    colorField(look, "shine", hc);
  }
  colorField(look, "bg", cJSON_GetObjectItem(a, "background_color"));
  auto numIn = [&](const char* k, float lo, float hi, std::initializer_list<const char*> dst, cJSON* obj) {
    const cJSON* v = cJSON_GetObjectItem(a, k);
    if (!cJSON_IsNumber(v)) return;
    float f = fminf(hi, fmaxf(lo, (float)v->valuedouble));
    for (const char* d : dst) cJSON_AddNumberToObject(obj, d, f);
  };
  numIn("eye_size", 0.6f, 1.3f, {"eye_w", "eye_h"}, look);
  numIn("iris_size", 0.5f, 1.5f, {"iris_w", "iris_h"}, look);
  numIn("eye_spacing", 0.75f, 1.25f, {"spacing"}, look);
  numIn("highlights", 0.f, 1.f, {"highlights"}, look);
  cJSON* disp = cJSON_AddObjectToObject(patch, "display");
  numIn("brightness", 5.f, 100.f, {"brightness"}, disp);
  cJSON* conv = cJSON_AddObjectToObject(patch, "conversation");
  numIn("volume", 0.f, 100.f, {"volume"}, conv);
  const cJSON* vo = cJSON_GetObjectItem(a, "voice");
  if (cJSON_IsString(vo) && strcmp(vo->valuestring, cv.voice)) {
    char v[24];
    snprintf(v, sizeof(v), "%s", vo->valuestring);
    for (char* q = v; *q; ++q) *q = (char)tolower((unsigned char)*q);
    cJSON_AddStringToObject(conv, "voice", v);
    restartVoice = true;  // the voice is fixed per session: continue in a new one
  }
  char* t = cJSON_PrintUnformatted(patch);
  cJSON_Delete(patch);
  bool ok = settings::apply(t);
  printf("change_look %s\n", t);
  cJSON_free(t);
  snprintf(result, cap, ok ? "{\"ok\":true,\"result\":\"Aussehen geaendert\"}" : "{\"ok\":false}");
  return ok;
}

// {"changes":[{"setting":"iris_color","value":"#ff0000"}, ...]} -> {"iris_color":"#ff0000", ...}
static cJSON* flattenChanges(const cJSON* a) {
  cJSON* o = cJSON_CreateObject();
  const cJSON* ch;
  cJSON_ArrayForEach(ch, cJSON_GetObjectItem(a, "changes")) {
    const cJSON* k = cJSON_GetObjectItem(ch, "setting");
    const cJSON* v = cJSON_GetObjectItem(ch, "value");
    if (!cJSON_IsString(k)) continue;
    const char* val = cJSON_IsString(v) ? v->valuestring : "";
    const char* key = k->valuestring;
    if (!strcmp(key, "reset")) {
      cJSON_AddBoolToObject(o, key, strcasecmp(val, "false") != 0);
    } else if (!strcmp(key, "colorway") || !strcmp(key, "voice") || strstr(key, "_color")) {
      cJSON_AddStringToObject(o, key, val);
    } else if (cJSON_IsNumber(v)) {
      cJSON_AddNumberToObject(o, key, v->valuedouble);
    } else {
      cJSON_AddNumberToObject(o, key, atof(val));
    }
  }
  return o;
}

// what the person said last (current turn, else the last one in the history), lower case ASCII
static void lastUserText(char* out, int cap) {
  xSemaphoreTake(txtMtx, portMAX_DELAY);
  snprintf(out, cap, "%s", inText);
  xSemaphoreGive(txtMtx);
  for (int i = histN - 1; i >= 0 && !out[0]; --i)
    if (hist[i].user) snprintf(out, cap, "%s", hist[i].text);
  for (char* q = out; *q; ++q) *q = (char)tolower((unsigned char)*q);
}

// sensitive self management only runs when the person asked for exactly that (the model alone may not)
static const char* sysAction(const ToolCall& c, char* buf, int cap) {
  buf[0] = 0;
  cJSON* a = c.args ? cJSON_Parse(c.args) : nullptr;
  const cJSON* ac = cJSON_GetObjectItem(a, "action");
  if (cJSON_IsString(ac)) snprintf(buf, cap, "%s", ac->valuestring);
  const cJSON* tx = cJSON_GetObjectItem(a, "text");
  // forgetting everything counts as clearing
  if (!strcmp(c.name, "memory") && !strcmp(buf, "forget") && cJSON_IsString(tx) && !strcasecmp(tx->valuestring, "alles"))
    snprintf(buf, cap, "forget_all");
  if (a) cJSON_Delete(a);
  return buf;
}
static bool askedFor(const char* action) {
  char said[420];
  lastUserText(said, sizeof(said));
  return guard::askedFor(action, said);
}
// Realtime transcribes what was said only after the model answered: a sensitive call waits up to 3 s
static bool holdCall(const ToolCall& c) {
  static char held[64];
  static int64_t since = 0;
  char act[24];
  if (strcmp(c.name, "system") && strcmp(c.name, "memory")) return false;
  if (askedFor(sysAction(c, act, sizeof(act)))) return false;
  int64_t now = esp_timer_get_time();
  if (strcmp(held, c.id)) {
    snprintf(held, sizeof(held), "%s", c.id);
    since = now;
  }
  return now - since < 3000000;
}

static void systemStatus(char* out, int cap) {
  char bat[48];
  if (battery::level() < 0)
    snprintf(bat, sizeof(bat), "kein Akku gemessen");
  else
    snprintf(bat, sizeof(bat), "Akku %d %%%s", battery::level(), battery::charging() ? " (laedt)" : "");
  int64_t up = esp_timer_get_time() / 60000000;
  char tl[200];
  assist::listTimers(tl, sizeof(tl));
  snprintf(out, cap,
           "Pixel Buddy Version " PIXEL_VERSION ", Laufzeit %lld h %lld min, WLAN %s (Signal %d dBm, IP %s), Firmware vom %s, freier Speicher %u KB intern / "
           "%u KB PSRAM, Kosten heute etwa %.2f Dollar, diesen Monat %.2f Dollar, %s Nachtmodus %s, %s.",
           up / 60, up % 60, net::ssid(), net::rssi(), net::ip(), esp_app_get_description()->date,
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024), assist::today(), assist::month(), tl,
           cv.nightOn ? "an" : "aus", bat);
}

static void toolResult(char* out, int cap, bool ok, const char* text) {
  cJSON* o = cJSON_CreateObject();
  cJSON_AddBoolToObject(o, "ok", ok);
  cJSON_AddStringToObject(o, "result", text);
  char* t = cJSON_PrintUnformatted(o);
  cJSON_Delete(o);
  snprintf(out, cap, "%s", t);
  cJSON_free(t);
}

// lamps: one way for set_light and smart_home (gentle off, remembered colours, eye animation)
static void lightTool(const cJSON* a, char* result, int cap) {
  char r[400];
  uint32_t rgb;
  const char* anim;
  bool ok = ha::setLight(cv, a, r, sizeof(r), rgb, anim);
  toolResult(result, cap, ok, r);
  printf("set_light -> %s\n", r);
  if (ok)
    post(EV_HOME, anim, rgb);
  else
    post(EV_ERROR, "cant");
}

static void runTool(const ToolCall& c, char* result, int cap) {
  cJSON* a = c.args ? cJSON_Parse(c.args) : nullptr;
  if (!strcmp(c.name, "change_look")) {
    if (cJSON_GetObjectItem(a, "changes")) {
      cJSON* flat = flattenChanges(a);
      cJSON_Delete(a);
      a = flat;
    }
    if (!changeLook(a, result, cap)) post(EV_ERROR, "cant");
  } else if (!strcmp(c.name, "play_animation")) {
    const cJSON* n = cJSON_GetObjectItem(a, "name");
    if (cJSON_IsString(n)) post(EV_PLAY, n->valuestring);
    snprintf(result, cap,
             "{\"ok\":true,\"result\":\"Die Animation wird jetzt vorgefuehrt - das ist nur eine Vorfuehrung, alles "
             "hat geklappt.\"}");
  } else if (!strcmp(c.name, "smart_home")) {
    const cJSON* e = cJSON_GetObjectItem(a, "entity_id");
    const cJSON* ac = cJSON_GetObjectItem(a, "action");
    const cJSON* v = cJSON_GetObjectItem(a, "value");
    const char* act = cJSON_IsString(ac) ? ac->valuestring : "";
    if (cJSON_IsString(e) && !strncmp(e->valuestring, "light.", 6) &&
        (!strcmp(act, "turn_on") || !strcmp(act, "turn_off") || !strcmp(act, "set_brightness"))) {
      cJSON* l = cJSON_CreateObject();  // a lamp: the same way as set_light
      cJSON_AddStringToObject(l, "entity_id", e->valuestring);
      cJSON_AddBoolToObject(l, "on", strcmp(act, "turn_off") != 0);
      if (!strcmp(act, "set_brightness") && cJSON_IsNumber(v)) cJSON_AddNumberToObject(l, "brightness_pct", v->valueint);
      lightTool(l, result, cap);
      cJSON_Delete(l);
    } else {
      char r[300] = "entity_id und action fehlen";
      bool ok = cJSON_IsString(e) && cJSON_IsString(ac) &&
                ha::control(cv, e->valuestring, act, cJSON_IsNumber(v) ? v->valuedouble : 0, cJSON_IsNumber(v), r,
                            sizeof(r));
      toolResult(result, cap, ok, r);
      printf("smart_home -> %s\n", r);
      if (!ok) post(EV_ERROR, "cant");
    }
  } else if (!strcmp(c.name, "end_conversation")) {
    post(EV_SYS, "ringstop");
    if (heardVoice) {
      endReq = true;
      post(EV_PLAY, "yes");  // a little nod, then quiet
      snprintf(result, cap, "{\"ok\":true}");
    } else {  // nobody really spoke: the transcription heard "nichts" in noise. Keep listening, silently
      noReply = true;
      snprintf(result, cap, "{\"ok\":false,\"result\":\"ignoriert\"}");
      ESP_LOGI(TAG, "end_conversation ignored: no voice heard yet");
    }
  } else if (!strcmp(c.name, "timer")) {
    const cJSON* ac = cJSON_GetObjectItem(a, "action");
    const cJSON* lb = cJSON_GetObjectItem(a, "label");
    const char* act = cJSON_IsString(ac) ? ac->valuestring : "";
    const char* label = cJSON_IsString(lb) ? lb->valuestring : "";
    auto num = [&](const char* k) {
      const cJSON* v = cJSON_GetObjectItem(a, k);
      return cJSON_IsNumber(v) ? v->valueint : -1;
    };
    char r[300];
    bool ok = true;
    if (!strcmp(act, "set")) {
      ok = assist::addTimer(num("seconds"), label);
      snprintf(r, sizeof(r), ok ? "Timer gestellt" : "Timer ging nicht (Dauer fehlt, zu viele Timer oder Uhrzeit unbekannt)");
      if (ok) post(EV_PLAY, "timer");
    } else if (!strcmp(act, "alarm")) {
      ok = assist::addAlarm(num("hour"), num("minute") < 0 ? 0 : num("minute"), label);
      snprintf(r, sizeof(r), ok ? "Wecker gestellt" : "Wecker ging nicht");
      if (ok) post(EV_PLAY, "timer");
    } else if (!strcmp(act, "cancel")) {
      post(EV_SYS, "ringstop");  // also the one that is ringing right now
      snprintf(r, sizeof(r), "%d geloescht", assist::cancel(label));
    } else {
      assist::listTimers(r, sizeof(r));
    }
    toolResult(result, cap, ok, r);
    if (!ok) post(EV_ERROR, "cant");
  } else if (!strcmp(c.name, "memory")) {
    const cJSON* ac = cJSON_GetObjectItem(a, "action");
    const cJSON* tx = cJSON_GetObjectItem(a, "text");
    char act[24];
    sysAction(c, act, sizeof(act));
    if (!cJSON_IsString(ac) || !cJSON_IsString(tx)) {
      snprintf(result, cap, "{\"ok\":false}");
    } else if (!strcmp(ac->valuestring, "remember")) {
      bool ok = assist::remember(tx->valuestring);
      toolResult(result, cap, ok, ok ? "gemerkt" : "Gedaechtnis voll - erst etwas vergessen");
    } else if (!askedFor(act)) {
      snprintf(result, cap, "{\"ok\":false,\"result\":\"Alles vergessen nur auf ausdrueckliche Bitte\"}");
    } else {
      int n = assist::forget(!strcmp(act, "forget_all") ? "" : tx->valuestring);
      snprintf(result, cap, "{\"ok\":true,\"result\":\"%d Eintraege vergessen\"}", n);
    }
  } else if (!strcmp(c.name, "show_weather")) {
    const cJSON* cd = cJSON_GetObjectItem(a, "condition");
    if (cJSON_IsString(cd)) post(EV_WEATHER, cd->valuestring);
    snprintf(result, cap, "{\"ok\":true,\"result\":\"wird angezeigt\"}");
  } else if (!strcmp(c.name, "system")) {
    char act[24];
    sysAction(c, act, sizeof(act));
    if (!askedFor(act)) {
      snprintf(result, cap, "{\"ok\":false,\"result\":\"Das mache ich nur, wenn du mich ausdruecklich darum bittest.\"}");
      ESP_LOGW(TAG, "system %s refused: not asked for explicitly", act);
    } else if (!strcmp(act, "status")) {
      char st[640];
      systemStatus(st, sizeof(st));
      toolResult(result, cap, true, st);
    } else if (!strcmp(act, "night_on") || !strcmp(act, "night_off")) {
      settings::apply(!strcmp(act, "night_on") ? "{\"night\":{\"on\":true}}" : "{\"night\":{\"on\":false}}");
      snprintf(result, cap, "{\"ok\":true}");
    } else if (!strcmp(act, "update_check") || !strcmp(act, "update_install")) {
      // look on GitHub (a few seconds), then report - or install it once the goodbye is said
      if (update::state() != update::AVAILABLE) {
        update::check(false);
        vTaskDelay(pdMS_TO_TICKS(300));
        for (int k = 0; k < 100 && update::state() == update::CHECKING; ++k) vTaskDelay(pdMS_TO_TICKS(200));
      }
      char r[300];
      update::State us = update::state();
      if (us == update::AVAILABLE && !strcmp(act, "update_install")) {
        snprintf(pendingSys, sizeof(pendingSys), "update");
        pendingSysAt = esp_timer_get_time();
        snprintf(r, sizeof(r), "Version %s wird gleich installiert, danach startest du neu. Sag nur ganz kurz tschuess.",
                 update::latest());
      } else if (us == update::AVAILABLE) {
        snprintf(r, sizeof(r), "Neue Version %s verfuegbar (installiert ist %s). Installieren nur, wenn der Nutzer es "
                 "ausdruecklich sagt.", update::latest(), PIXEL_VERSION);
      } else if (us == update::UP_TO_DATE) {
        snprintf(r, sizeof(r), "Alles aktuell: Version %s ist die neueste.", PIXEL_VERSION);
      } else {
        snprintf(r, sizeof(r), "Konnte gerade nicht nach Updates suchen (GitHub nicht erreichbar).");
      }
      toolResult(result, cap, us != update::FAILED, r);
    } else if (!strcmp(act, "clear_history")) {
      remove("/sdcard/pixel/chat.log");
      forgetFlag = true;
      snprintf(result, cap, "{\"ok\":true,\"result\":\"Verlauf geloescht\"}");
    } else {  // sleep, privacy, restart, shutdown: after the short answer
      snprintf(pendingSys, sizeof(pendingSys), "%s", act);
      pendingSysAt = esp_timer_get_time();
      snprintf(result, cap, "{\"ok\":true,\"result\":\"passiert gleich, sag nur ganz kurz tschuess\"}");
    }
  } else if (!strcmp(c.name, "set_light")) {
    lightTool(a, result, cap);
  } else {
    snprintf(result, cap, "{\"ok\":false,\"result\":\"unbekanntes Werkzeug\"}");
  }
  if (a) cJSON_Delete(a);
}

static bool searchPending = false;

static void sendToolOutput(const char* callId, const char* result) {
  cJSON* r = cJSON_CreateObject();
  cJSON_AddStringToObject(r, "type", rt ? "conversation.item.create" : "response.item.create");
  cJSON* it = cJSON_AddObjectToObject(r, "item");
  cJSON_AddStringToObject(it, "type", "function_call_output");
  cJSON_AddStringToObject(it, "call_id", callId);
  cJSON_AddStringToObject(it, "output", result);
  char* t = cJSON_PrintUnformatted(r);
  cJSON_Delete(r);
  sendJson(t);
  cJSON_free(t);
}

// Realtime: the model called tools; answer them and let it continue (web search answers later)
static void serveToolsRealtime() {
  bool any = false;
  while (callR != callW) {
    ToolCall& c = calls[callR % 6];
    static char result[600];
    if (holdCall(c)) break;
    if (!strcmp(c.name, "web_search")) {
      cJSON* a = c.args ? cJSON_Parse(c.args) : nullptr;
      const cJSON* q = cJSON_GetObjectItem(a, "query");
      EXT_RAM_BSS_ATTR static Job j;
      j.kind = JOB_SEARCH;
      snprintf(j.callId, sizeof(j.callId), "%s", c.id);
      snprintf(j.text, sizeof(j.text), "%s", cJSON_IsString(q) ? q->valuestring : "");
      if (a) cJSON_Delete(a);
      searchReady = false;
      searchPending = true;
      delegations = delegations + 1;
      post(EV_BUSY, "1");
      xQueueSend(jobQ, &j, 0);
    } else {
      runTool(c, result, sizeof(result));
      sendToolOutput(c.id, result);
      any = true;
    }
    free(c.args);
    c.args = nullptr;
    callR = callR + 1;
  }
  if (searchPending && searchReady) {
    searchPending = false;
    searchReady = false;
    sendToolOutput(searchCall, searchOut);
    if (delegations > 0) delegations = delegations - 1;
    post(EV_BUSY, "0");
    any = true;
  }
  if (any && !searchPending && !endReq && !noReply) sendJson("{\"type\":\"response.create\"}");
  noReply = false;
}

// answer pending tool calls, then let the backend continue
static void serveTools() {
  if (rt) {
    serveToolsRealtime();
    return;
  }
  bool any = false;
  while (callR != callW) {
    ToolCall& c = calls[callR % 6];
    static char result[600];
    if (holdCall(c)) break;
    runTool(c, result, sizeof(result));
    sendToolOutput(c.id, result);
    free(c.args);
    c.args = nullptr;
    callR = callR + 1;
    any = true;
  }
  if (any && !endReq && !noReply) sendJson("{\"type\":\"response.create\"}");
  noReply = false;
}

// ------------------------------------------------------------------ session
static void endSession() {
  if (wsUp && started && !rt) {
    sendJson("{\"type\":\"session.close\"}");
    for (int i = 0; i < 40 && !closedEv && wsUp; ++i) vTaskDelay(pdMS_TO_TICKS(50));
  }
  if (rt && wsUp && started && !restartVoice) {  // kept for the next conversation (idle costs nothing)
    if (rtResponding) sendJson("{\"type\":\"response.cancel\"}", 300);
    sendJson("{\"type\":\"input_audio_buffer.clear\"}", 300);
  } else {
    dropSocket();  // a fresh warm socket is opened right after
  }
  xSemaphoreTake(txtMtx, portMAX_DELAY);
  if (inText[0]) pushTurn(true, inText);
  if (outText[0]) pushTurn(false, outText);
  xSemaphoreGive(txtMtx);
  mic::setUplink(false);
  mic::streamFlush();
  while (callR != callW) {
    free(calls[callR % 6].args);
    callR = callR + 1;
  }
  if (delegations) post(EV_BUSY, "0");
  delegations = 0;
  searchPending = false;
  rtResponding = false;
  if (rt) {  // the helper may still answer the last eye reaction, then it closes its connection
    static Job c;
    c.kind = JOB_CLOSE;
    xQueueSend(jobQ, &c, 0);
  }
  wasStarted = started;
  started = false;
  pausedAtUs = esp_timer_get_time();
  ph = IDLE;
}

static void runSession() {
  int64_t t0 = esp_timer_get_time();
  restartVoice = false;
  endReq = false;
  settings::conv(cv);
  rt = !strncmp(cv.model, "gpt-realtime", 12);
  if (wsUp && strcmp(wsModel, cv.model)) dropSocket();  // the model was changed: the warm socket is for the old one
  mic::setStreamContinuous(!rt);
  if (rt) {
    upRs.init(3, 2, 7600.f, 16000.f);    // microphone 16 -> 24 kHz
    downRs.init(2, 3, 7400.f, 24000.f);  // speech 24 -> 16 kHz
  }
  if (forgetFlag) {
    forgetFlag = false;
    histN = 0;
  }
  heardVoice = typedPending;
  pendingSys[0] = 0;
  rtItem[0] = truncMsg[0] = 0;
  liveDrop = false;
  interruptReq = false;
  bool resume = histN > 0 && t0 - pausedAtUs < (int64_t)cv.resumeS * 1000000;
  // Realtime: a new conversation or another voice (fixed once it has spoken) needs a fresh session
  if (rt && wsUp && rtHasConv && (!resume || strcmp(rtSessVoice, rtVoice(cv.voice)))) dropSocket();
  if (!resume) {
    histN = 0;
    struct stat st;  // keep the log small: the old half goes to chat.old.log
    if (stat("/sdcard/pixel/chat.log", &st) == 0 && st.st_size > 512 * 1024) {
      remove("/sdcard/pixel/chat.old.log");
      rename("/sdcard/pixel/chat.log", "/sdcard/pixel/chat.old.log");
    }
    sd::append("/sdcard/pixel/chat.log", "\n--- neues Gespraech ---\n");
  }
  inText[0] = outText[0] = 0;
  userSpokeFlag = false;
  started = false;
  closedEv = false;
  startError[0] = 0;
  delegations = 0;
  newTurn();

  if (!net::connected() || !settings::apiKey()[0] || assist::overBudget(cv.budgetDay)) {
    // no network / no key (setup not done) / the daily budget is used up
    post(EV_ERROR, !net::connected() ? "offline" : !settings::apiKey()[0] ? "locked" : "quota");
    if (assist::overBudget(cv.budgetDay)) ESP_LOGW(TAG, "daily budget of %.2f USD reached", cv.budgetDay);
    mic::setUplink(false);
    typedPending = false;
    ph = IDLE;
    return;
  }
  bool warm = wsUp;
  if (!wsUp && !connectSocket(cv.model)) {
    post(EV_ERROR, failKind(handshakeStatus, nullptr));
    mic::setUplink(false);
    ph = IDLE;
    return;
  }
  bool sent = sendStart(resume);
  for (int i = 0; i < 120 && sent && !started && !startError[0] && wsUp; ++i) vTaskDelay(pdMS_TO_TICKS(25));
  if (!started && !startError[0] && warm) {  // the warm socket had gone stale: once more on a fresh one
    ESP_LOGW(TAG, "warm connection did not answer, reconnecting");
    if (!connectSocket(cv.model)) {
      post(EV_ERROR, failKind(handshakeStatus, nullptr));
      endSession();
      return;
    }
    sent = sendStart(resume);
  }
  for (int i = 0; i < 160 && sent && !started && !startError[0] && wsUp; ++i) vTaskDelay(pdMS_TO_TICKS(25));
  if (!started) {
    ESP_LOGW(TAG, "session did not start (%s)", startError[0] ? startError : wsUp ? "timeout" : "connection lost");
    post(EV_ERROR, failKind(handshakeStatus, startError[0] ? startError : "timeout"));
    endSession();
    return;
  }
  ESP_LOGI(TAG, "session started %d ms after the wake word (%s)", (int)((esp_timer_get_time() - t0) / 1000),
           resume ? "continuing" : "new conversation");
  if (rt) {  // eye reactions and web search need the helper connection soon
    static Job w;
    w.kind = JOB_WARM;
    xQueueSend(jobQ, &w, 0);
  }
  ph = WAITING;

  EXT_RAM_BSS_ATTR static int16_t chunk[3200];
  EXT_RAM_BSS_ATTR static int16_t chunk24[4900];
  static char* frame = (char*)heap_caps_malloc(16384, MALLOC_CAP_SPIRAM);
  const int FRAME_CAP = 16384;
  int64_t quietSince = esp_timer_get_time();
  int64_t lastUserVoice = 0;
  bool lost = false, abortNow = false;
  uint32_t sessVer = 0;  // compare once right away: the settings may have changed while connecting
  for (;;) {
    // microphone -> server. The first reads drain what was said while connecting (up to
    // 200 ms per message), afterwards every 40 ms.
    // 40 ms per message (half the TLS records of 20 ms); the backlog after connecting goes in bigger pieces
    vTaskDelay(pdMS_TO_TICKS(40));
    int n = mic::uplinkRead(chunk, rt ? 3000 : 3200, 0);
    if (n > 0) {
      size_t olen = 0;
      const int16_t* pcm = chunk;
      if (rt) {  // Realtime takes 24 kHz
        n = upRs.run(chunk, n, chunk24);
        pcm = chunk24;
      }
      int pre = snprintf(frame, FRAME_CAP, "{\"type\":\"%s\",\"audio\":\"",
                         rt ? "input_audio_buffer.append" : "session.input_audio.append");
      mbedtls_base64_encode((unsigned char*)frame + pre, FRAME_CAP - pre - 4, &olen, (const unsigned char*)pcm, n * 2);
      memcpy(frame + pre + olen, "\"}", 3);
      // the receiving side may hold the client for a moment (Realtime sends whole answers in a burst):
      // a missed microphone chunk is no reason to give up, only a socket that stays unusable
      static int64_t failSince = 0;
      if (!sendJson(frame, 300)) {
        if (!failSince) failSince = esp_timer_get_time();
        if (!wsUp || esp_timer_get_time() - failSince > 6000000) {
          failSince = 0;
          lost = true;
          break;
        }
      } else {
        failSince = 0;
      }
    }
    if (!wsUp || closedEv) {
      lost = !closedEv;
      break;
    }
    serveTools();
    if (endReq) {
      ESP_LOGI(TAG, "the person ended the conversation");
      break;
    }
    if (truncMsg[0]) {  // the interrupted answer: tell the server how much of it was heard
      sendJson(truncMsg, 300);
      truncMsg[0] = 0;
    }
    if (interruptReq) {  // touched while it talks
      interruptReq = false;
      if (rt && rtResponding) sendJson("{\"type\":\"response.cancel\"}", 300);
      cutAnswer();
      ESP_LOGI(TAG, "interrupted by touch");
    }
    if (typedPending) {  // typed in the web interface: the person's turn as text
      typedPending = false;
      cJSON* e = cJSON_CreateObject();
      cJSON_AddStringToObject(e, "type", rt ? "conversation.item.create" : "response.item.create");
      cJSON* it = cJSON_AddObjectToObject(e, "item");
      cJSON_AddStringToObject(it, "type", "message");
      cJSON_AddStringToObject(it, "role", "user");
      cJSON* part = cJSON_CreateObject();
      cJSON_AddStringToObject(part, "type", "input_text");
      cJSON_AddStringToObject(part, "text", typed);
      cJSON_AddItemToArray(cJSON_AddArrayToObject(it, "content"), part);
      char* t = cJSON_PrintUnformatted(e);
      cJSON_Delete(e);
      if (rt && rtResponding) sendJson("{\"type\":\"response.cancel\"}", 300);
      sendJson(t);
      sendJson("{\"type\":\"response.create\"}");
      cJSON_free(t);
      xSemaphoreTake(txtMtx, portMAX_DELAY);
      if (inText[0]) pushTurn(true, inText);
      pushTurn(true, typed);
      xSemaphoreGive(txtMtx);
      userSpokeFlag = heardVoice = true;
      lastInUs = esp_timer_get_time();
    }

    // who is talking? (local voice detection reacts faster than the transcript)
    int64_t now = esp_timer_get_time();
    // a turn is complete after 1.5 s without new text from that side
    xSemaphoreTake(txtMtx, portMAX_DELAY);
    if (inText[0] && now - lastInUs > 1500000) pushTurn(true, inText);
    if (outText[0] && now - lastOutUs > 1500000) pushTurn(false, outText);
    xSemaphoreGive(txtMtx);
    if (mic::get().voice && !mic::uplinkMuted()) lastUserVoice = now;
    if (lastUserVoice) heardVoice = true;
    bool user = now - lastUserVoice < 600000 || now - lastInUs < 1200000;
    bool robot = mic::audible() || rtResponding || now - lastOutUs < 1000000;
    if (user || robot || delegations > 0) quietSince = now;
    ph = robot ? RESPONDING : user ? USER_TALKING : WAITING;
    // right after the wake word give some more time for the first sentence, afterwards pause_s
    float limit = (userSpokeFlag || lastUserVoice) ? cv.pauseS : fmaxf(cv.pauseS, 8.f);
    if (now - quietSince > (int64_t)(limit * 1000000)) break;  // pause: the wake word continues
    if (restartVoice && !robot && delegations == 0 && now - lastOutUs > 600000) break;
    // self management (sleep, restart ...): once the short answer is through, at the latest after 8 s
    if (pendingSys[0] && ((!robot && delegations == 0 && now - pendingSysAt > 1500000) || now - pendingSysAt > 8000000))
      break;
    // voice or model changed in the web interface: stop right away and go on in a new session
    if (settings::version() != sessVer) {
      sessVer = settings::version();
      EXT_RAM_BSS_ATTR static settings::Conv nc;
      settings::conv(nc);
      if (!restartVoice && (strcmp(nc.voice, cv.voice) || strcmp(nc.model, cv.model))) {
        ESP_LOGI(TAG, "voice/model changed in the web interface: new session");
        restartVoice = abortNow = true;
      }
    }
    if (abortNow) break;
    if (now - t0 > 600000000LL) break;                              // 10 minute safety limit
  }
  if (lost) {
    ESP_LOGW(TAG, "connection lost during the session");
    post(EV_ERROR, net::connected() ? "glitch" : "offline");
  }
  endSession();
  ESP_LOGI(TAG, "session ended (wake word within %d s continues it)", cv.resumeS);
}

void forget() { forgetFlag = true; }

static void task(void*) {
  msg = (char*)heap_caps_malloc(MSG_CAP, MALLOC_CAP_SPIRAM);
  pcmTmp = (uint8_t*)heap_caps_malloc(PCM_CAP, MALLOC_CAP_SPIRAM);
  rsTmp = (int16_t*)heap_caps_malloc(PCM_CAP, MALLOC_CAP_SPIRAM);
  settings::conv(cv);
  homeDevices = (char*)heap_caps_malloc(HOME_CAP, MALLOC_CAP_SPIRAM);
  homeDevices[0] = 0;
  int64_t nextConnect = 0, nextHome = 0;
  int backoff = 0;
  uint32_t seenVer = 0;
  for (;;) {
    // wake word?
    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(250))) {
      runSession();
      for (int k = 0; k < 3 && restartVoice && net::connected(); ++k) {  // new voice: go on seamlessly
        ESP_LOGI(TAG, "voice changed, continuing in a new session");
        ph = CONNECTING;
        mic::setUplink(true);
        runSession();
      }
      // every activation is kept for the wake word (at most KWS_MAX_FILES recordings on the card):
      // - somebody spoke afterwards and it was "Pixel": the 2 s up to the detection, a new example of the word
      // - nobody said anything (no words in the transcript): all 6 s as a false wake-up (negative example)
      if (byWake && wasStarted && (!userSpokeFlag || mic::wakeClipPixel())) {
        EXT_RAM_BSS_ATTR static int16_t clip[16000 * 6];
        int n = mic::wakeClip(clip, sizeof(clip) / 2);
        const char* kind = userSpokeFlag ? "ap" : "fa";
        if (userSpokeFlag && n > 32000) n = 32000;
        if (sd::countFiles("/sdcard/pixel/kws", ".wav") >= mic::KWS_MAX_FILES)
          ESP_LOGW(TAG, "%d wake word recordings on the card: no new ones", mic::KWS_MAX_FILES);
        else if (n && sd::saveWavLater("/sdcard/pixel/kws", kind, clip, n))
          ESP_LOGI(TAG, "%s kept for the wake word (kws/%s_*.wav)", userSpokeFlag ? "\"Pixel\"" : "false wake-up?", kind);
      }
      if (pendingSys[0]) {
        ESP_LOGI(TAG, "self management: %s", pendingSys);
        if (!strcmp(pendingSys, "restart")) {
          vTaskDelay(pdMS_TO_TICKS(300));
          esp_restart();
        }
        post(EV_SYS, pendingSys);  // sleep, privacy, shutdown: the main loop
        pendingSys[0] = 0;
      }
      nextConnect = esp_timer_get_time() + 300000;  // re-open the warm socket shortly
      nextHome = esp_timer_get_time() + 1000000;    // and refresh the device states (they may have changed)
      backoff = 0;
      continue;
    }
    if (weatherReq) {  // morning greeting: the weather from Home Assistant on the eyes
      weatherReq = false;
      char cond[24];
      settings::conv(cv);
      if (ha::weather(cv, cond, sizeof(cond))) post(EV_WEATHER, cond);
    }
    int64_t now = esp_timer_get_time();
    if (!net::connected()) {
      if (ws) dropSocket();
      continue;
    }
    if (settings::version() != seenVer) {  // settings changed: maybe another model
      seenVer = settings::version();
      settings::conv(cv);
      if (wsUp && strcmp(wsModel, cv.model)) {
        ESP_LOGI(TAG, "model changed to %s", cv.model);
        dropSocket();
        nextConnect = now;
      }
    }
    // a kept Realtime conversation is over (resume_s passed): a fresh session for the next one
    if (wsUp && rtHasConv && now - pausedAtUs > (int64_t)cv.resumeS * 1000000) {
      ESP_LOGI(TAG, "conversation over, fresh session");
      dropSocket();
      nextConnect = now;
    }
    // a Realtime session lasts at most an hour: renew the idle one in time
    if (wsUp && !strncmp(wsModel, "gpt-realtime", 12) && now - wsOpenedUs > 50LL * 60 * 1000000) dropSocket();
    // keep a warm socket so the next session starts immediately
    // a socket the server closes again right away (e.g. no credit left) counts as a failed try,
    // otherwise it reconnects every few seconds forever
    if (wsUp && backoff && now - wsOpenedUs > 30000000) backoff = 0;
    if (!wsUp && now >= nextConnect && settings::apiKey()[0]) {
      rt = !strncmp(cv.model, "gpt-realtime", 12);
      if (connectSocket(cv.model)) ESP_LOGI(TAG, "ready (warm connection, %s)", cv.model);
      static const int waits[] = {3, 10, 30, 60, 120};
      nextConnect = esp_timer_get_time() + (int64_t)waits[backoff] * 1000000;
      if (backoff < 4) ++backoff;
    }
    // smart home device list for the next session (HTTP takes a moment, so not after the wake word)
    if (now >= nextHome) {
      nextHome = now + 60000000LL;
      settings::conv(cv);
      if (ha::configured(cv)) {
        int nDev = ha::listDevices(cv, homeDevices, HOME_CAP);
        ESP_LOGI(TAG, "smart home: %d devices", nDev);
        if (nDev < 0) homeDevices[0] = 0;
      } else {
        homeDevices[0] = 0;
      }
    }
  }
}

void start() {
  evQ = xQueueCreate(12, sizeof(Event));
  txtMtx = xSemaphoreCreateMutex();
  TRIGGER_OUT_REF = TRIGGER_OUT;
  jobQ = xQueueCreate(3, sizeof(Job));
  searchOut = (char*)heap_caps_malloc(SEARCH_CAP, MALLOC_CAP_SPIRAM);
  searchOut[0] = 0;
  // core 0: its TLS handshake (software crypto, ~1 s of CPU) on core 1 halved the frame rate at every
  // session start. Core 0 only renders in short bursts (and listens while no session runs), so even at
  // priority 4 the handshake gets enough time there (on core 1 below the frame task it starved).
  xTaskCreatePinnedToCoreWithCaps(helperTask, "oai_helper", 8192, nullptr, 4, nullptr, 0, MALLOC_CAP_SPIRAM);
  // above the WebSocket task (5): its sends get the client lock as soon as the receiver lets go.
  // Core 0 too: at priority 6 on core 1 it pre-empted the frame task, core 0 only loses render bands.
  xTaskCreatePinnedToCoreWithCaps(task, "live", 10240, nullptr, 6, &taskH, 0, MALLOC_CAP_SPIRAM);
}
void begin(bool wakeWord) {
  if (ph != IDLE || !taskH) return;
  byWake = wakeWord;
  ph = CONNECTING;
  mic::setUplink(true);  // buffer everything from the wake word on
  xTaskNotifyGive(taskH);
}
bool active() { return ph != IDLE; }
Phase phase() { return ph; }
bool popEvent(Event& e) { return evQ && xQueueReceive(evQ, &e, 0) == pdTRUE; }
void say(const char* text) {
  snprintf(typed, sizeof(typed), "%s", text);
  utf8Trim(typed);
  typedPending = true;  // sent as soon as the session runs (the main loop starts one if needed)
}
void interrupt() {
  if (ph == RESPONDING || mic::audible()) interruptReq = true;
}
void requestWeather() { weatherReq = true; }

}  // namespace live
