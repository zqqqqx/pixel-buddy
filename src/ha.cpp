#include "ha.h"
#include "net.h"

#include <initializer_list>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_attr.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

namespace ha {

static const char* TAG = "ha";

bool configured(const settings::Conv& c) { return c.haUrl[0] && c.haToken[0]; }
// hotspot mode: Home Assistant is at home, out of reach. The tools stay and say so.
static const char* AWAY = "Pixel is on the go in hotspot mode - the smart home only works at home on the home WiFi.";

// one request; response text (0-terminated) in resp, returns HTTP status or -1
static int call(const settings::Conv& c, const char* method, const char* path, const char* body, char* resp, int cap) {
  char url[200], auth[340];
  snprintf(url, sizeof(url), "%s%s", c.haUrl, path);
  snprintf(auth, sizeof(auth), "Bearer %s", c.haToken);
  esp_http_client_config_t hc = {};
  hc.url = url;
  hc.timeout_ms = 6000;
  hc.crt_bundle_attach = esp_crt_bundle_attach;  // for https (e.g. Nabu Casa)
  hc.method = strcmp(method, "POST") ? HTTP_METHOD_GET : HTTP_METHOD_POST;
  esp_http_client_handle_t h = esp_http_client_init(&hc);
  esp_http_client_set_header(h, "Authorization", auth);
  esp_http_client_set_header(h, "Content-Type", "application/json");
  int len = body ? strlen(body) : 0;
  int status = -1;
  if (esp_http_client_open(h, len) == ESP_OK) {
    if (len) esp_http_client_write(h, body, len);
    esp_http_client_fetch_headers(h);
    status = esp_http_client_get_status_code(h);
    int got = 0, r;
    while (resp && got < cap - 1 && (r = esp_http_client_read(h, resp + got, cap - 1 - got)) > 0) got += r;
    if (resp) resp[got] = 0;
  }
  esp_http_client_cleanup(h);
  if (status != 200 && status != 201) ESP_LOGW(TAG, "%s %s -> %d", method, path, status);
  return status;
}

int listDevices(const settings::Conv& c, char* out, int cap) {
  if (!configured(c)) return -1;
  if (net::hotspot()) {
    snprintf(out, cap, "(none reachable: %s)", AWAY);
    return 0;
  }
  const char* tpl =
      "{\"template\":\"{% for s in states if s.domain in ['light','switch','cover','climate','fan','media_player',"
      "'lock','scene','script','input_boolean','vacuum','weather'] or (s.domain == 'sensor' and s.attributes.device_class "
      "in ['temperature','humidity']) %}{{ s.entity_id }}|{{ s.name }}|{{ s.state }}{{ s.attributes.unit_of_measurement "
      "| default('') }}{% if s.domain == 'weather' %}|{{ s.attributes.temperature }}{{ s.attributes.temperature_unit "
      "}}{% endif %}{% if s.domain == 'light' %}|{{ s.attributes.supported_color_modes | default([]) | join(',') }}"
      "{% if s.attributes.min_color_temp_kelvin %}|{{ s.attributes.min_color_temp_kelvin }}-"
      "{{ s.attributes.max_color_temp_kelvin }}K{% endif %}{% if s.attributes.effect_list %}|Effekte: "
      "{{ s.attributes.effect_list | join(', ') }}{% endif %}{% endif %}\\n{% endfor %}\"}";
  int st = call(c, "POST", "/api/template", tpl, out, cap);
  if (st != 200) {
    out[0] = 0;
    return -1;
  }
  int lines = 0;
  for (char* p = out; *p; ++p)
    if (*p == '\n') lines++;
  if ((int)strlen(out) >= cap - 2) ESP_LOGW(TAG, "device list cut at %d bytes: the rest is unknown to the model", cap);
  return lines;
}

// Some lamps (e.g. Tuya) report color_mode color_temp even while they shine in colour. For those the
// colour set last is remembered: reported instead of the state, and sent along with a brightness
// change (brightness alone may switch them back to white).
struct Mem {
  char entity[48];
  uint32_t rgb;
  bool hasRgb, misreports;
};
static Mem mem[8];
static Mem& memFor(const char* entity) {
  for (auto& m : mem)
    if (!strcmp(m.entity, entity)) return m;
  for (auto& m : mem)
    if (!m.entity[0]) {
      snprintf(m.entity, sizeof(m.entity), "%s", entity);
      return m;
    }
  static Mem spare;  // shortcut: more than 8 colour lamps share one slot, raise the table size then
  spare = {};
  return spare;
}
static uint32_t kelvinRgb(double k) {  // rough white tone for the eyes: warm 2000 K .. cold 6500 K
  float t = (float)((k - 2000) / 4500);
  t = t < 0 ? 0 : t > 1 ? 1 : t;
  auto ch = [&](float a, float b) { return (uint32_t)(a + (b - a) * t); };
  return ch(255, 225) << 16 | ch(150, 235) << 8 | ch(60, 255);
}

bool weather(const settings::Conv& c, char* cond, int cap) {
  if (!configured(c) || net::hotspot()) return false;
  char out[64];
  const char* tpl = "{\"template\":\"{{ (states.weather | first).state if states.weather | list | count else '' }}\"}";
  if (call(c, "POST", "/api/template", tpl, out, sizeof(out)) != 200 || !out[0]) return false;
  snprintf(cond, cap, "%s", out);
  return true;
}

// ------------------------------------------------------------------ live state changes (WebSocket API)
// Lamps switched elsewhere (app, wall switch, automations) show on the eyes, and the irises can follow
// one lamp's colour. Changes Pixel made itself in the last seconds are not shown twice.
static QueueHandle_t evq = nullptr;
EXT_RAM_BSS_ATTR static char wsBuf[16384];
static int wsLen = 0;
static esp_websocket_client_handle_t hws = nullptr;
static volatile int wsStep = 0;  // 0 closed, 1 waiting for auth, 2 subscribed
static settings::Conv* evConv = nullptr;
static struct {
  char entity[48];
  int64_t at;
} own[4];

static void wsHandle(const char* m) {
  if (strstr(m, "\"auth_required\"")) {
    static char auth[360];
    snprintf(auth, sizeof(auth), "{\"type\":\"auth\",\"access_token\":\"%s\"}", evConv->haToken);
    esp_websocket_client_send_text(hws, auth, strlen(auth), pdMS_TO_TICKS(1000));
  } else if (strstr(m, "\"auth_ok\"")) {
    const char* sub = "{\"id\":1,\"type\":\"subscribe_events\",\"event_type\":\"state_changed\"}";
    esp_websocket_client_send_text(hws, sub, strlen(sub), pdMS_TO_TICKS(1000));
    wsStep = 2;
    ESP_LOGI(TAG, "listening for lamp changes");
  } else if (strstr(m, "\"auth_invalid\"")) {
    ESP_LOGW(TAG, "event stream: token rejected");
  } else if (strstr(m, "\"entity_id\":\"light.")) {  // only lamps are parsed (sensors change all the time)
    cJSON* j = cJSON_Parse(m);
    const cJSON* d = cJSON_GetObjectItem(cJSON_GetObjectItem(j, "event"), "data");
    const cJSON* id = cJSON_GetObjectItem(d, "entity_id");
    const cJSON* ns = cJSON_GetObjectItem(d, "new_state");
    const cJSON* os = cJSON_GetObjectItem(d, "old_state");
    const cJSON* st = cJSON_GetObjectItem(ns, "state");
    const cJSON* ost = cJSON_GetObjectItem(os, "state");
    if (cJSON_IsString(id) && cJSON_IsString(st)) {
      LightEvent e = {};
      snprintf(e.entity, sizeof(e.entity), "%s", id->valuestring);
      e.on = !strcmp(st->valuestring, "on");
      const cJSON* at = cJSON_GetObjectItem(ns, "attributes");
      const cJSON* rc = cJSON_GetObjectItem(at, "rgb_color");
      const cJSON* orc = cJSON_GetObjectItem(cJSON_GetObjectItem(os, "attributes"), "rgb_color");
      e.rgb = 0xFFC46E;
      if (cJSON_GetArraySize(rc) == 3)
        e.rgb = (uint32_t)cJSON_GetArrayItem(rc, 0)->valueint << 16 | cJSON_GetArrayItem(rc, 1)->valueint << 8 |
                cJSON_GetArrayItem(rc, 2)->valueint;
      for (auto& m : mem)  // a lamp that misreports its colour: the one Pixel set
        if (!strcmp(m.entity, e.entity) && m.hasRgb && m.misreports) e.rgb = m.rgb;
      bool wasOn = cJSON_IsString(ost) && !strcmp(ost->valuestring, "on");
      char* a = rc ? cJSON_PrintUnformatted(rc) : nullptr;
      char* b = orc ? cJSON_PrintUnformatted(orc) : nullptr;
      e.change = e.on != wasOn ? (e.on ? 1 : 2) : (a && b && strcmp(a, b)) ? 3 : 0;  // on / off / colour
      cJSON_free(a);
      cJSON_free(b);
      int64_t now = esp_timer_get_time();
      for (auto& o : own)
        if (!strcmp(o.entity, e.entity) && now - o.at < 5000000) e.change = 0;  // Pixel did that itself
      if (evq) xQueueSend(evq, &e, 0);
    }
    if (j) cJSON_Delete(j);
  }
}

static void onWs(void*, esp_event_base_t, int32_t id, void* data) {
  auto* d = (esp_websocket_event_data_t*)data;
  if (id == WEBSOCKET_EVENT_DISCONNECTED || id == WEBSOCKET_EVENT_CLOSED) wsStep = 0;
  if (id != WEBSOCKET_EVENT_DATA || (d->op_code != 0x01 && d->op_code != 0x00)) return;
  if (d->payload_offset == 0) wsLen = 0;
  if (wsLen + d->data_len < (int)sizeof(wsBuf) - 1) {
    memcpy(wsBuf + wsLen, d->data_ptr, d->data_len);
    wsLen += d->data_len;
  } else {
    wsLen = sizeof(wsBuf);  // too big (some huge entity): skipped
  }
  if (d->payload_offset + d->data_len >= d->payload_len && wsLen < (int)sizeof(wsBuf)) {
    wsBuf[wsLen] = 0;
    wsHandle(wsBuf);
  }
}

static void eventTask(void*) {
  EXT_RAM_BSS_ATTR static settings::Conv c;
  evConv = &c;
  static char url[160];
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(5000));
    settings::conv(c);
    bool want = configured(c) && net::connected() && !net::hotspot();
    if (hws && (!want || wsStep == 0)) {  // gone, or the settings changed: start over
      esp_websocket_client_destroy(hws);
      hws = nullptr;
    }
    if (!want || hws) continue;
    const char* host = strstr(c.haUrl, "://");
    snprintf(url, sizeof(url), "%s://%s/api/websocket", strncmp(c.haUrl, "https", 5) ? "ws" : "wss",
             host ? host + 3 : c.haUrl);
    esp_websocket_client_config_t cfg = {};
    cfg.uri = url;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.buffer_size = 1024;  // messages are put together in wsBuf (PSRAM); internal RAM is scarce
    cfg.task_stack = 4096;
    cfg.task_core_id_set = true;  // core 1 belongs to the eyes
    cfg.task_core_id = 0;
    cfg.reconnect_timeout_ms = 15000;
    cfg.network_timeout_ms = 8000;
    hws = esp_websocket_client_init(&cfg);
    esp_websocket_register_events(hws, WEBSOCKET_EVENT_ANY, onWs, nullptr);
    wsStep = 1;
    if (esp_websocket_client_start(hws) != ESP_OK) wsStep = 0;
    vTaskDelay(pdMS_TO_TICKS(25000));  // give it time to connect before judging it
  }
}

void startEvents() {
  evq = xQueueCreate(6, sizeof(LightEvent));
  xTaskCreatePinnedToCoreWithCaps(eventTask, "ha_events", 4096, nullptr, 3, nullptr, 1, MALLOC_CAP_SPIRAM);
}
bool popLight(LightEvent& e) { return evq && xQueueReceive(evq, &e, 0) == pdTRUE; }

// Off, but gently: lamps that can fade get a 2 s transition, the others are dimmed down in three steps
// (a lamp that misreports its colour gets the colour along, or a brightness step turns it white)
static void softOff(const settings::Conv& c, const char* entity, cJSON* body, const Mem& m) {
  char path[80];
  snprintf(path, sizeof(path), "/api/states/%s", entity);
  EXT_RAM_BSS_ATTR static char resp[4096];
  if (call(c, "GET", path, nullptr, resp, sizeof(resp)) != 200) return;
  cJSON* j = cJSON_Parse(resp);
  const cJSON* st = cJSON_GetObjectItem(j, "state");
  const cJSON* at = cJSON_GetObjectItem(j, "attributes");
  const cJSON* f = cJSON_GetObjectItem(at, "supported_features");
  const cJSON* b = cJSON_GetObjectItem(at, "brightness");
  bool isOn = cJSON_IsString(st) && !strcmp(st->valuestring, "on");
  if (isOn && cJSON_IsNumber(f) && (f->valueint & 32)) {  // LightEntityFeature.TRANSITION
    cJSON_AddNumberToObject(body, "transition", 2);
  } else if (isOn && cJSON_IsNumber(b) && b->valuedouble > 8) {
    int pct = (int)(b->valuedouble * 100 / 255);
    for (float k : {0.6f, 0.35f, 0.15f}) {
      cJSON* s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "entity_id", entity);
      cJSON_AddNumberToObject(s, "brightness_pct", pct * k < 1 ? 1 : (int)(pct * k));
      if (m.hasRgb && m.misreports) {
        int v[3] = {(int)(m.rgb >> 16), (int)(m.rgb >> 8 & 255), (int)(m.rgb & 255)};
        cJSON_AddItemToObject(s, "rgb_color", cJSON_CreateIntArray(v, 3));
      }
      char* t = cJSON_PrintUnformatted(s);
      cJSON_Delete(s);
      call(c, "POST", "/api/services/light/turn_on", t, nullptr, 0);
      cJSON_free(t);
      vTaskDelay(pdMS_TO_TICKS(250));
    }
  }
  if (j) cJSON_Delete(j);
}

bool setLight(const settings::Conv& c, const cJSON* a, char* result, int cap, uint32_t& rgb, const char*& anim) {
  rgb = 0xFFC46E;
  anim = "light_on";
  const cJSON* e = cJSON_GetObjectItem(a, "entity_id");
  if (configured(c) && net::hotspot()) {
    snprintf(result, cap, "%s", AWAY);
    return false;
  }
  if (!configured(c) || !cJSON_IsString(e) || strncmp(e->valuestring, "light.", 6) || strlen(e->valuestring) >= 48) {
    snprintf(result, cap, configured(c) ? "entity_id must be a light.* from the list" : "No smart home is set up.");
    return false;
  }
  Mem& m = memFor(e->valuestring);
  if (m.hasRgb) rgb = m.rgb;
  const cJSON* on = cJSON_GetObjectItem(a, "on");
  cJSON* body = cJSON_CreateObject();
  cJSON_AddStringToObject(body, "entity_id", e->valuestring);
  const cJSON* col = cJSON_GetObjectItem(a, "rgb_color");
  const cJSON* kel = cJSON_GetObjectItem(a, "color_temp_kelvin");
  const cJSON* eff = cJSON_GetObjectItem(a, "effect");
  bool off = cJSON_IsFalse(on);
  if (off) {
    anim = "light_off";
    softOff(c, e->valuestring, body, m);
  } else {
    if (cJSON_IsArray(col) && cJSON_GetArraySize(col) == 3) {
      int v[3];
      for (int i = 0; i < 3; ++i) {
        const cJSON* x = cJSON_GetArrayItem(col, i);
        v[i] = cJSON_IsNumber(x) ? (x->valueint < 0 ? 0 : x->valueint > 255 ? 255 : x->valueint) : 0;
      }
      cJSON_AddItemToObject(body, "rgb_color", cJSON_CreateIntArray(v, 3));
      rgb = (uint32_t)v[0] << 16 | v[1] << 8 | v[2];
      anim = "light_color";
    } else if (cJSON_IsNumber(kel)) {
      cJSON_AddNumberToObject(body, "color_temp_kelvin", kel->valueint);
      rgb = kelvinRgb(kel->valuedouble);
      anim = "light_color";
    } else if (cJSON_IsString(eff)) {
      cJSON_AddStringToObject(body, "effect", eff->valuestring);
      anim = "light_color";
    } else if (m.hasRgb && m.misreports) {
      int v[3] = {(int)(m.rgb >> 16), (int)(m.rgb >> 8 & 255), (int)(m.rgb & 255)};
      cJSON_AddItemToObject(body, "rgb_color", cJSON_CreateIntArray(v, 3));
    }
    for (const char* k : {"brightness_pct", "brightness_step_pct"}) {
      const cJSON* x = cJSON_GetObjectItem(a, k);
      if (cJSON_IsNumber(x)) cJSON_AddNumberToObject(body, k, x->valueint);
    }
  }
  char* b = cJSON_PrintUnformatted(body);
  cJSON_Delete(body);
  EXT_RAM_BSS_ATTR static char resp[8192];
  int st = call(c, "POST", off ? "/api/services/light/turn_off" : "/api/services/light/turn_on", b, resp, sizeof(resp));
  if (st != 200) {
    snprintf(result, cap, "Home Assistant refused (error %d)%s", st,
             st == 400 ? " - this lamp probably cannot do that (colour, white tone or effect)" : "");
    cJSON_free(b);
    return false;
  }
  bool newColour = !strcmp(anim, "light_color");
  if (newColour) {  // remember it (a white tone or an effect forgets it)
    m.hasRgb = cJSON_IsArray(col);
    m.rgb = rgb;
  }
  // the new state: does the lamp report its colour honestly, and which colour does it have now?
  cJSON* r = off ? nullptr : cJSON_Parse(resp);
  const cJSON* s;
  cJSON_ArrayForEach(s, r) {
    const cJSON* id = cJSON_GetObjectItem(s, "entity_id");
    const cJSON* at = cJSON_GetObjectItem(s, "attributes");
    const cJSON* mode = cJSON_GetObjectItem(at, "color_mode");
    const cJSON* rc = cJSON_GetObjectItem(at, "rgb_color");
    if (!cJSON_IsString(id) || strcmp(id->valuestring, e->valuestring)) continue;
    if (newColour && m.hasRgb && cJSON_IsString(mode))
      m.misreports = !strcmp(mode->valuestring, "color_temp") || !strcmp(mode->valuestring, "white");
    if (!newColour && !(m.hasRgb && m.misreports) && cJSON_GetArraySize(rc) == 3)
      rgb = (uint32_t)cJSON_GetArrayItem(rc, 0)->valueint << 16 | cJSON_GetArrayItem(rc, 1)->valueint << 8 |
            cJSON_GetArrayItem(rc, 2)->valueint;
  }
  if (r) cJSON_Delete(r);
  for (auto& o : own)  // the event stream will report this change: not shown again
    if (!o.entity[0] || !strcmp(o.entity, e->valuestring) || esp_timer_get_time() - o.at > 5000000) {
      snprintf(o.entity, sizeof(o.entity), "%s", e->valuestring);
      o.at = esp_timer_get_time();
      break;
    }
  snprintf(result, cap, "Done: %s", b);
  cJSON_free(b);
  return true;
}

bool control(const settings::Conv& c, const char* entity, const char* action, double value, bool hasValue,
             char* result, int cap) {
  if (!configured(c)) {
    snprintf(result, cap, "No smart home is set up.");
    return false;
  }
  if (net::hotspot()) {
    snprintf(result, cap, "%s", AWAY);
    return false;
  }
  char domain[32] = {0};
  const char* dot = strchr(entity, '.');
  if (!dot || dot - entity >= (int)sizeof(domain)) {
    snprintf(result, cap, "Unknown device %s - use the entity_id from the list.", entity);
    return false;
  }
  memcpy(domain, entity, dot - entity);

  static char resp[2048];
  if (!strcmp(action, "get_state")) {
    char path[160];
    snprintf(path, sizeof(path), "/api/states/%s", entity);
    if (call(c, "GET", path, nullptr, resp, sizeof(resp)) != 200) {
      snprintf(result, cap, "Could not ask %s.", entity);
      return false;
    }
    cJSON* j = cJSON_Parse(resp);
    const cJSON* s = cJSON_GetObjectItem(j, "state");
    const cJSON* a = cJSON_GetObjectItem(j, "attributes");
    const cJSON* u = cJSON_GetObjectItem(a, "unit_of_measurement");
    const cJSON* t = cJSON_GetObjectItem(a, "current_temperature");
    const cJSON* b = cJSON_GetObjectItem(a, "brightness");
    snprintf(result, cap, "%s: %s%s%s", entity, cJSON_IsString(s) ? s->valuestring : "?",
             cJSON_IsString(u) ? u->valuestring : "", "");
    if (cJSON_IsNumber(t)) snprintf(result + strlen(result), cap - strlen(result), ", currently %.1f degrees", t->valuedouble);
    if (cJSON_IsNumber(b))
      snprintf(result + strlen(result), cap - strlen(result), ", brightness %d%%", (int)(b->valuedouble * 100 / 255));
    const Mem* m = nullptr;
    for (auto& x : mem)
      if (!strcmp(x.entity, entity) && x.hasRgb && x.misreports) m = &x;
    const cJSON* rc = cJSON_GetObjectItem(a, "rgb_color");
    const cJSON* k = cJSON_GetObjectItem(a, "color_temp_kelvin");
    const cJSON* mode = cJSON_GetObjectItem(a, "color_mode");
    const cJSON* fx = cJSON_GetObjectItem(a, "effect");
    int n = strlen(result);
    if (m)
      snprintf(result + n, cap - n, ", colour RGB %d,%d,%d", (int)(m->rgb >> 16), (int)(m->rgb >> 8 & 255), (int)(m->rgb & 255));
    else if (cJSON_IsString(mode) && !strcmp(mode->valuestring, "color_temp") && cJSON_IsNumber(k))
      snprintf(result + n, cap - n, ", white tone %d K", k->valueint);
    else if (cJSON_GetArraySize(rc) == 3)
      snprintf(result + n, cap - n, ", colour RGB %d,%d,%d", cJSON_GetArrayItem(rc, 0)->valueint,
               cJSON_GetArrayItem(rc, 1)->valueint, cJSON_GetArrayItem(rc, 2)->valueint);
    n = strlen(result);
    if (cJSON_IsString(fx)) snprintf(result + n, cap - n, ", effect %s", fx->valuestring);
    if (j) cJSON_Delete(j);
    return true;
  }

  // map the generic action to a Home Assistant service
  const char* service = action;
  const char* svcDomain = domain;
  cJSON* body = cJSON_CreateObject();
  cJSON_AddStringToObject(body, "entity_id", entity);
  bool cover = !strcmp(domain, "cover"), lock = !strcmp(domain, "lock");
  if (!strcmp(action, "set_brightness")) {
    svcDomain = "light";
    service = "turn_on";
    cJSON_AddNumberToObject(body, "brightness_pct", hasValue ? value : 100);
  } else if (!strcmp(action, "set_temperature")) {
    svcDomain = "climate";
    service = "set_temperature";
    cJSON_AddNumberToObject(body, "temperature", hasValue ? value : 21);
  } else if (!strcmp(action, "set_position")) {
    service = "set_cover_position";
    cJSON_AddNumberToObject(body, "position", hasValue ? value : 50);
  } else if (cover && (!strcmp(action, "open") || !strcmp(action, "turn_on"))) {
    service = "open_cover";
  } else if (cover && (!strcmp(action, "close") || !strcmp(action, "turn_off"))) {
    service = "close_cover";
  } else if (lock) {
    service = (!strcmp(action, "turn_on") || !strcmp(action, "close")) ? "lock" : "unlock";
  } else if (!strcmp(domain, "scene") || !strcmp(domain, "script")) {
    service = "turn_on";
  } else if (!strcmp(action, "open")) {
    service = "turn_on";
  } else if (!strcmp(action, "close")) {
    service = "turn_off";
  }
  if (!strcmp(domain, "light") && !strcmp(service, "turn_off")) cJSON_AddNumberToObject(body, "transition", 2);
  char path[120];
  snprintf(path, sizeof(path), "/api/services/%s/%s", svcDomain, service);
  char* b = cJSON_PrintUnformatted(body);
  cJSON_Delete(body);
  int st = call(c, "POST", path, b, resp, sizeof(resp));
  cJSON_free(b);
  if (st == 200) {
    snprintf(result, cap, "Erledigt: %s.%s fuer %s", svcDomain, service, entity);
    return true;
  }
  snprintf(result, cap, "Home Assistant hat %s mit Fehler %d abgelehnt.", entity, st);
  return false;
}

}  // namespace ha
