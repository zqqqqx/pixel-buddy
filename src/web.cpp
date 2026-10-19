#include "web.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mdns.h"
#include "cJSON.h"
#include "sd.h"
#include "ha.h"
#include "live.h"
#include "mic.h"
#include <dirent.h>
#include <initializer_list>
#include <sys/stat.h>
#include "settings.h"
#include "update.h"
#include "esp_app_desc.h"
#include "web_ui.h"
#include "eyes.h"
#include "screen.h"
#include "net.h"
#include "esp_ota_ops.h"
#include "lwip/sockets.h"

namespace web {

static const char* TAG = "web";
static QueueHandle_t cmdQ;
static SemaphoreHandle_t stMtx;
static char statusJson[700] = "{}";

static esp_err_t sendJson(httpd_req_t* r, const char* s) {
  httpd_resp_set_type(r, "application/json");
  httpd_resp_set_hdr(r, "Cache-Control", "no-store");
  return httpd_resp_send(r, s, HTTPD_RESP_USE_STRLEN);
}

// read the request body into a PSRAM buffer (caller frees)
static char* body(httpd_req_t* r) {
  if (r->content_len <= 0 || r->content_len > 64 * 1024) return nullptr;
  char* b = (char*)heap_caps_malloc(r->content_len + 1, MALLOC_CAP_SPIRAM);
  int got = 0;
  while (got < (int)r->content_len) {
    int n = httpd_req_recv(r, b + got, r->content_len - got);
    if (n <= 0) {
      free(b);
      return nullptr;
    }
    got += n;
  }
  b[got] = 0;
  return b;
}

// Every POST must say Content-Type application/json (the firmware upload: octet-stream). A form on a
// foreign web page cannot send these, and fetch() from one needs a CORS preflight this server never
// allows: so a page you visit cannot change settings, reset or flash the robot behind your back.
static esp_err_t guarded(httpd_req_t* r) {
  char ct[48] = "";
  httpd_req_get_hdr_value_str(r, "Content-Type", ct, sizeof(ct));
  if (!strstr(ct, "application/json") && !strstr(ct, "application/octet-stream")) {
    httpd_resp_set_status(r, "415 Unsupported Media Type");
    return httpd_resp_send(r, "Content-Type: application/json", HTTPD_RESP_USE_STRLEN);
  }
  return ((esp_err_t(*)(httpd_req_t*))r->user_ctx)(r);
}

static esp_err_t hSetup(httpd_req_t* r);
static esp_err_t hIndex(httpd_req_t* r) {
  // a phone in the setup access point gets the setup page
  int fd = httpd_req_to_sockfd(r);
  sockaddr_in6 a = {};
  socklen_t al = sizeof(a);
  if (net::setupActive() && getsockname(fd, (sockaddr*)&a, &al) == 0) {
    uint32_t ip4 = a.sin6_family == AF_INET6 ? a.sin6_addr.un.u32_addr[3] : ((sockaddr_in*)&a)->sin_addr.s_addr;
    if ((ip4 & 0xFFFFFF) == (192 | 168 << 8 | 4 << 16)) return hSetup(r);  // we were reached as 192.168.4.x
  }
  httpd_resp_set_type(r, "text/html; charset=utf-8");
  httpd_resp_set_hdr(r, "Content-Encoding", "gzip");
  return httpd_resp_send(r, (const char*)WEB_UI_GZ, WEB_UI_GZ_LEN);
}
static esp_err_t hGetConfig(httpd_req_t* r) {
  char* j = settings::toJson();
  esp_err_t e = sendJson(r, j);
  free(j);
  return e;
}
static esp_err_t hPostConfig(httpd_req_t* r) {
  char* b = body(r);
  bool ok = b && settings::apply(b);
  free(b);
  if (!ok) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "bad json");
  return sendJson(r, "{\"ok\":true}");
}
static esp_err_t hReset(httpd_req_t* r) {
  settings::reset();
  return sendJson(r, "{\"ok\":true}");
}
static esp_err_t hStatus(httpd_req_t* r) {
  static char copy[sizeof(statusJson)];
  xSemaphoreTake(stMtx, portMAX_DELAY);
  memcpy(copy, statusJson, sizeof(copy));
  xSemaphoreGive(stMtx);
  return sendJson(r, copy);
}
static esp_err_t hPlay(httpd_req_t* r) {
  char* b = body(r);
  cJSON* j = b ? cJSON_Parse(b) : nullptr;
  free(b);
  const cJSON* n = j ? cJSON_GetObjectItem(j, "name") : nullptr;
  if (cJSON_IsString(n)) {
    char c[40];
    snprintf(c, sizeof(c), "play:%s", n->valuestring);
    xQueueSend(cmdQ, c, 0);
  }
  if (j) cJSON_Delete(j);
  return sendJson(r, "{\"ok\":true}");
}
static esp_err_t hWake(httpd_req_t* r) {
  char c[40] = "wake";
  xQueueSend(cmdQ, c, 0);
  return sendJson(r, "{\"ok\":true}");
}
static esp_err_t hLog(httpd_req_t* r) {
  char q[16] = "";
  if (httpd_req_get_url_query_str(r, q, sizeof(q)) == ESP_OK && strstr(q, "all")) {  // the whole log, to download
    httpd_resp_set_type(r, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(r, "Content-Disposition", "attachment; filename=\"pixel-gespraeche.txt\"");
    static char chunk[4096];
    for (const char* p : {"/sdcard/pixel/chat.old.log", "/sdcard/pixel/chat.log"}) {
      FILE* f = fopen(p, "rb");
      if (!f) continue;
      size_t n;
      while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) httpd_resp_send_chunk(r, chunk, n);
      fclose(f);
    }
    return httpd_resp_send_chunk(r, nullptr, 0);
  }
  const long cap = 24 * 1024;
  char* t = (char*)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
  sd::readTail("/sdcard/pixel/chat.log", t, cap);
  httpd_resp_set_type(r, "text/plain; charset=utf-8");
  esp_err_t e = httpd_resp_send(r, t, HTTPD_RESP_USE_STRLEN);
  free(t);
  return e;
}
static esp_err_t hLogClear(httpd_req_t* r) {
  remove("/sdcard/pixel/chat.log");
  live::forget();  // the robot forgets the conversation too
  return sendJson(r, "{\"ok\":true}");
}
static esp_err_t hHaTest(httpd_req_t* r) {
  static settings::Conv c;  // big: not on the server task stack
  settings::conv(c);
  const int cap = 4096;
  char* t = (char*)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
  int n = ha::listDevices(c, t, cap);
  cJSON* j = cJSON_CreateObject();
  cJSON_AddNumberToObject(j, "devices", n);
  cJSON_AddStringToObject(j, "list", t);
  free(t);
  char* s = cJSON_PrintUnformatted(j);
  cJSON_Delete(j);
  esp_err_t e = sendJson(r, s);
  cJSON_free(s);
  return e;
}
// ---- wake word dataset: recordings from the robot's own microphone, kept on the SD card
static const char* KWS_DIR = "/sdcard/pixel/kws";
static esp_err_t hKwsRecord(httpd_req_t* r) {
  char q[40] = "", kind[8] = "pos";
  if (httpd_req_get_url_query_str(r, q, sizeof(q)) == ESP_OK) httpd_query_key_value(q, "kind", kind, sizeof(kind));
  bool bg = !strcmp(kind, "bg");
  if (!sd::ok()) return sendJson(r, "{\"ok\":false,\"error\":\"keine SD-Karte\"}");
  if (live::active()) return sendJson(r, "{\"ok\":false,\"error\":\"gerade läuft ein Gespräch – kurz warten\"}");
  if (sd::countFiles(KWS_DIR, ".wav") >= mic::KWS_MAX_FILES)
    return sendJson(r, "{\"ok\":false,\"error\":\"800 Aufnahmen – erst am PC sichern und automatische löschen\"}");
  const int n = bg ? 16000 * 10 : 16000 * 5 / 2;  // "Pixel": 0.5 s before the click + 2 s, background: 10 s
  int16_t* pcm = (int16_t*)heap_caps_malloc(n * 2, MALLOC_CAP_SPIRAM);
  mic::setWake(false);  // saying "Pixel" must not start a conversation that talks into the next recording
  int got = mic::record(pcm, n);
  if (!live::active()) mic::setWake(true);
  int peak = 0;
  for (int i = 0; i < got; ++i) peak = abs(pcm[i]) > peak ? abs(pcm[i]) : peak;
  char name[40] = "";
  bool ok = sd::saveWav(KWS_DIR, bg ? "bg" : "pos", pcm, got, name, sizeof(name));
  free(pcm);
  char j[160];
  snprintf(j, sizeof(j), "{\"ok\":%s,\"file\":\"%s\",\"peak\":%d}", ok ? "true" : "false", name, peak);
  return sendJson(r, j);
}
static esp_err_t hKwsList(httpd_req_t* r) {
  cJSON* a = cJSON_CreateArray();
  DIR* d = opendir(KWS_DIR);
  if (d) {
    struct dirent* e;
    while ((e = readdir(d))) if (strstr(e->d_name, ".wav")) cJSON_AddItemToArray(a, cJSON_CreateString(e->d_name));
    closedir(d);
  }
  char* s = cJSON_PrintUnformatted(a);
  cJSON_Delete(a);
  esp_err_t er = sendJson(r, s);
  cJSON_free(s);
  return er;
}
static esp_err_t hKwsFile(httpd_req_t* r) {
  char q[64] = "", name[40] = "";
  if (httpd_req_get_url_query_str(r, q, sizeof(q)) == ESP_OK) httpd_query_key_value(q, "name", name, sizeof(name));
  if (!name[0] || strchr(name, '/') || strstr(name, "..")) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "name");
  char path[80];
  snprintf(path, sizeof(path), "%s/%s", KWS_DIR, name);
  FILE* f = fopen(path, "rb");
  if (!f) return httpd_resp_send_err(r, HTTPD_404_NOT_FOUND, "missing");
  httpd_resp_set_type(r, "audio/wav");
  static char chunk[4096];
  size_t n;
  while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) httpd_resp_send_chunk(r, chunk, n);
  fclose(f);
  return httpd_resp_send_chunk(r, nullptr, 0);
}
static esp_err_t hKwsDelete(httpd_req_t* r) {
  char q[64] = "", name[40] = "";
  if (httpd_req_get_url_query_str(r, q, sizeof(q)) == ESP_OK) httpd_query_key_value(q, "name", name, sizeof(name));
  if (!name[0] || strchr(name, '/') || strstr(name, "..")) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "name");
  char path[80];
  snprintf(path, sizeof(path), "%s/%s", KWS_DIR, name);
  return sendJson(r, remove(path) == 0 ? "{\"ok\":true}" : "{\"ok\":false}");
}

// listened to in the web interface: "Pixel" -> pv_<name> (positive), "not Pixel" -> nv_<name> (negative).
// A long false wake-up clip marked as "Pixel" keeps only its first 2 s (up to the detection).
static esp_err_t hKwsMark(httpd_req_t* r) {
  char q[96] = "", name[40] = "", as[8] = "";
  if (httpd_req_get_url_query_str(r, q, sizeof(q)) == ESP_OK) {
    httpd_query_key_value(q, "name", name, sizeof(name));
    httpd_query_key_value(q, "as", as, sizeof(as));
  }
  bool pixel = !strcmp(as, "pixel");
  if (!name[0] || strchr(name, '/') || strstr(name, "..") || (!pixel && strcmp(as, "not")))
    return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "name");
  const char* base = !strncmp(name, "pv_", 3) || !strncmp(name, "nv_", 3) ? name + 3 : name;
  char from[80], to[80], nn[48];
  snprintf(nn, sizeof(nn), "%s_%s", pixel ? "pv" : "nv", base);
  snprintf(from, sizeof(from), "%s/%s", KWS_DIR, name);
  snprintf(to, sizeof(to), "%s/%s", KWS_DIR, nn);
  if (!strcmp(from, to)) return sendJson(r, "{\"ok\":true}");
  FILE* f = fopen(from, "rb");
  if (!f) return httpd_resp_send_err(r, HTTPD_404_NOT_FOUND, "missing");
  fseek(f, 0, SEEK_END);
  long len = ftell(f);
  fclose(f);
  const long keep = 44 + 16000 * 2 * 2;  // header + 2 s
  bool ok;
  if (pixel && len > keep + 16000) {  // trim: copy the first 2 s into the new file
    uint8_t* b = (uint8_t*)heap_caps_malloc(keep, MALLOC_CAP_SPIRAM);
    f = fopen(from, "rb");
    ok = b && f && fread(b, 1, keep, f) == (size_t)keep;
    if (f) fclose(f);
    if (ok) {
      uint32_t data = keep - 44, riff = keep - 8;
      memcpy(b + 4, &riff, 4);
      memcpy(b + 40, &data, 4);
      FILE* o = fopen(to, "wb");
      ok = o && fwrite(b, 1, keep, o) == (size_t)keep;
      if (o) fclose(o);
      if (ok) remove(from);
    }
    free(b);
  } else {
    ok = rename(from, to) == 0;
  }
  char j[80];
  snprintf(j, sizeof(j), "{\"ok\":%s,\"file\":\"%s\"}", ok ? "true" : "false", nn);
  return sendJson(r, j);
}

// the automatically collected ones (ap_ "Pixel", fa_ false wake-ups, pv_ / nv_ checked), e.g. once on the PC
static esp_err_t hKwsClear(httpd_req_t* r) {
  int removed = 0;
  if (DIR* d = opendir(KWS_DIR)) {
    char path[300];
    while (struct dirent* e = readdir(d)) {
      bool autom = false;
      for (const char* p : {"ap_", "fa_", "pv_", "nv_"}) autom |= !strncmp(e->d_name, p, 3);
      if (!autom) continue;
      snprintf(path, sizeof(path), "%s/%s", KWS_DIR, e->d_name);
      removed += remove(path) == 0;
    }
    closedir(d);
  }
  char j[48];
  snprintf(j, sizeof(j), "{\"ok\":true,\"removed\":%d}", removed);
  return sendJson(r, j);
}

// ---- setup (own access point): the page, nearby networks, store the network and the OpenAI key
static esp_err_t hSetup(httpd_req_t* r) {
  httpd_resp_set_type(r, "text/html; charset=utf-8");
  httpd_resp_set_hdr(r, "Content-Encoding", "gzip");
  httpd_resp_set_hdr(r, "Cache-Control", "no-store");
  return httpd_resp_send(r, (const char*)SETUP_UI_GZ, SETUP_UI_GZ_LEN);
}
static esp_err_t hWifiScan(httpd_req_t* r) {
  char* out = (char*)heap_caps_malloc(3072, MALLOC_CAP_SPIRAM);
  net::scan(out, 3072);
  esp_err_t e = sendJson(r, out);
  free(out);
  return e;
}
static esp_err_t hWifiSave(httpd_req_t* r) {
  char* b = body(r);
  cJSON* j = b ? cJSON_Parse(b) : nullptr;
  free(b);
  const cJSON* ss = cJSON_GetObjectItem(j, "ssid");
  const cJSON* pw = cJSON_GetObjectItem(j, "pass");
  const cJSON* key = cJSON_GetObjectItem(j, "api_key");
  bool ok = cJSON_IsString(ss) && net::saveWifi(ss->valuestring, cJSON_IsString(pw) ? pw->valuestring : "");
  if (ok && cJSON_IsString(key) && key->valuestring[0]) {
    cJSON* c = cJSON_CreateObject();
    cJSON_AddStringToObject(cJSON_AddObjectToObject(c, "conversation"), "api_key", key->valuestring);
    char* t = cJSON_PrintUnformatted(c);
    cJSON_Delete(c);
    settings::apply(t);
    cJSON_free(t);
  }
  if (j) cJSON_Delete(j);
  return sendJson(r, ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"Netzname fehlt oder enthält \\\"/\\\\, oder Passwort zu kurz (mind. 8 Zeichen)\"}");
}
// hotspot mode network (switched on in the BOOT 5x menu); the password never goes back to the page
static esp_err_t hHotspot(httpd_req_t* r) {
  char* b = body(r);
  cJSON* j = b ? cJSON_Parse(b) : nullptr;
  free(b);
  const cJSON* ss = cJSON_GetObjectItem(j, "ssid");
  const cJSON* pw = cJSON_GetObjectItem(j, "pass");
  bool ok = cJSON_IsString(ss) && net::saveHotspot(ss->valuestring, cJSON_IsString(pw) ? pw->valuestring : "");
  if (j) cJSON_Delete(j);
  return sendJson(r, ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"Netzname enthält \\\"/\\\\, oder Passwort zu kurz (mind. 8 Zeichen)\"}");
}
static esp_err_t hWifiStatus(httpd_req_t* r) {
  char j[200];
  snprintf(j, sizeof(j), "{\"state\":%d,\"ip\":\"%s\",\"ssid\":\"%s\",\"setup\":%s,\"key\":%s}", net::joinStatus(),
           net::ip(), net::ssid(), net::setupActive() ? "true" : "false", settings::apiKey()[0] ? "true" : "false");
  return sendJson(r, j);
}
// captive portal: phones probe some address on the internet; in the setup everything leads to the page
static esp_err_t hNotFound(httpd_req_t* r, httpd_err_code_t) {
  if (!net::setupActive()) return httpd_resp_send_err(r, HTTPD_404_NOT_FOUND, "not found");
  httpd_resp_set_status(r, "302 Found");
  httpd_resp_set_hdr(r, "Location", "http://192.168.4.1/setup");
  return httpd_resp_send(r, nullptr, 0);
}

// ---- firmware update: the .bin from the build (web interface -> System)
// ---- updates from GitHub: state, look now, install what was found
static esp_err_t hUpdate(httpd_req_t* r) {
  char j[900];
  update::json(j, sizeof(j));
  return sendJson(r, j);
}
static esp_err_t hUpdateCheck(httpd_req_t* r) {
  update::check(false);
  return hUpdate(r);
}
static esp_err_t hUpdateInstall(httpd_req_t* r) {
  update::install();
  return hUpdate(r);
}

// ---- settings backup on the SD card: made before every firmware update (and on request)
static void backupNow() {
  const esp_app_desc_t* a = esp_app_get_description();
  char fw[40];
  snprintf(fw, sizeof(fw), "%s %s", a->date, a->time);
  settings::backup(fw);
}
static esp_err_t hBackupInfo(httpd_req_t* r) {
  long saved = 0;
  char fw[40] = "", j[120];
  bool ok = settings::backupInfo(saved, fw, sizeof(fw));
  snprintf(j, sizeof(j), "{\"exists\":%s,\"saved\":%ld,\"build\":\"%s\"}", ok ? "true" : "false", saved, fw);
  return sendJson(r, j);
}
static esp_err_t hBackupSave(httpd_req_t* r) {
  backupNow();
  return hBackupInfo(r);
}
static esp_err_t hBackupRestore(httpd_req_t* r) {
  return sendJson(r, settings::restoreBackup() ? "{\"ok\":true}" : "{\"ok\":false}");
}

static esp_err_t hOta(httpd_req_t* r) {
  const esp_partition_t* part = esp_ota_get_next_update_partition(nullptr);
  if (part && r->content_len >= 100000) backupNow();  // the settings as they were, in case the new one loses them
  esp_ota_handle_t h;
  if (!part || r->content_len < 100000 || esp_ota_begin(part, r->content_len, &h) != ESP_OK)
    return sendJson(r, "{\"ok\":false,\"error\":\"Update nicht moeglich\"}");
  EXT_RAM_BSS_ATTR static char buf[4096];  // flash writes from PSRAM go through a bounce buffer
  int left = r->content_len;
  bool ok = true;
  while (left > 0 && ok) {
    int n = httpd_req_recv(r, buf, left < (int)sizeof(buf) ? left : sizeof(buf));
    if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
    ok = n > 0 && esp_ota_write(h, buf, n) == ESP_OK;
    left -= n;
  }
  ok = ok && esp_ota_end(h) == ESP_OK;  // checks the image
  if (!ok) {
    if (left > 0) esp_ota_abort(h);
    return sendJson(r, "{\"ok\":false,\"error\":\"Datei ungueltig oder Uebertragung abgebrochen\"}");
  }
  esp_ota_set_boot_partition(part);
  sendJson(r, "{\"ok\":true}");
  ESP_LOGI(TAG, "firmware update written to %s, restarting", part->label);
  vTaskDelay(pdMS_TO_TICKS(500));
  esp_restart();
  return ESP_OK;
}

// ---- live preview for the web interface: the current frame, 120 x 120 RGB565 (little endian)
static esp_err_t hScreen(httpd_req_t* r) {
  static uint16_t* full = (uint16_t*)heap_caps_malloc(240 * 240 * 2, MALLOC_CAP_SPIRAM);
  static uint16_t* half = (uint16_t*)heap_caps_malloc(120 * 120 * 2, MALLOC_CAP_SPIRAM);
  if (!full || !half) return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "memory");
  UBaseType_t prio = uxTaskPriorityGet(nullptr);
  vTaskPrioritySet(nullptr, 3);  // below the render task: the preview takes only spare time, not frames
  if (screen::active())
    screen::render(full, 0, 0, 240, 240);
  else
    eyes::render(full, 0, 0, 240, 240);  // reads the latest frame (a torn one now and then is fine here)
  vTaskPrioritySet(nullptr, prio);
  for (int y = 0; y < 120; ++y)
    for (int x = 0; x < 120; ++x) {
      int r5 = 0, g6 = 0, b5 = 0;
      for (int k = 0; k < 4; ++k) {
        uint16_t c = full[(2 * y + (k >> 1)) * 240 + 2 * x + (k & 1)];
        c = (uint16_t)(c >> 8 | c << 8);  // the display wants swapped bytes
        r5 += c >> 11;
        g6 += c >> 5 & 63;
        b5 += c & 31;
      }
      half[y * 120 + x] = (uint16_t)((r5 / 4) << 11 | (g6 / 4) << 5 | (b5 / 4));
    }
  httpd_resp_set_type(r, "application/octet-stream");
  httpd_resp_set_hdr(r, "Cache-Control", "no-store");
  return httpd_resp_send(r, (const char*)half, 120 * 120 * 2);
}

// ---- typed message to the robot
static esp_err_t hChat(httpd_req_t* r) {
  char* b = body(r);
  cJSON* j = b ? cJSON_Parse(b) : nullptr;
  free(b);
  const cJSON* t = cJSON_GetObjectItem(j, "text");
  bool ok = cJSON_IsString(t) && t->valuestring[0];
  if (ok) {
    live::say(t->valuestring);
    char c[40] = "chat";
    xQueueSend(cmdQ, c, 0);
  }
  if (j) cJSON_Delete(j);
  return sendJson(r, ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

static esp_err_t hRestart(httpd_req_t* r) {
  sendJson(r, "{\"ok\":true}");
  vTaskDelay(pdMS_TO_TICKS(300));
  esp_restart();
  return ESP_OK;
}

void start() {
  cmdQ = xQueueCreate(4, 40);
  stMtx = xSemaphoreCreateMutex();
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.stack_size = 7168;
  cfg.max_uri_handlers = 40;
  cfg.lru_purge_enable = true;
  cfg.max_open_sockets = 5;  // leave sockets for the conversation (WebSocket, helper HTTPS)
  cfg.core_id = 0;
  httpd_handle_t srv = nullptr;
  if (httpd_start(&srv, &cfg) != ESP_OK) {
    ESP_LOGE(TAG, "http server failed");
    return;
  }
  const httpd_uri_t uris[] = {
      {"/", HTTP_GET, hIndex, nullptr},
      {"/api/config", HTTP_GET, hGetConfig, nullptr},
      {"/api/config", HTTP_POST, guarded, (void*)hPostConfig},
      {"/api/reset", HTTP_POST, guarded, (void*)hReset},
      {"/api/status", HTTP_GET, hStatus, nullptr},
      {"/api/play", HTTP_POST, guarded, (void*)hPlay},
      {"/api/wake", HTTP_POST, guarded, (void*)hWake},
      {"/api/log", HTTP_GET, hLog, nullptr},
      {"/api/log/clear", HTTP_POST, guarded, (void*)hLogClear},
      {"/api/restart", HTTP_POST, guarded, (void*)hRestart},
      {"/api/ha/test", HTTP_GET, hHaTest, nullptr},
      {"/api/kws/record", HTTP_POST, guarded, (void*)hKwsRecord},
      {"/api/kws/list", HTTP_GET, hKwsList, nullptr},
      {"/api/kws/file", HTTP_GET, hKwsFile, nullptr},
      {"/api/kws/delete", HTTP_POST, guarded, (void*)hKwsDelete},
      {"/api/kws/clear", HTTP_POST, guarded, (void*)hKwsClear},
      {"/api/kws/mark", HTTP_POST, guarded, (void*)hKwsMark},
      {"/api/backup", HTTP_GET, hBackupInfo, nullptr},
      {"/api/update", HTTP_GET, hUpdate, nullptr},
      {"/api/update/check", HTTP_POST, guarded, (void*)hUpdateCheck},
      {"/api/update/install", HTTP_POST, guarded, (void*)hUpdateInstall},
      {"/api/backup", HTTP_POST, guarded, (void*)hBackupSave},
      {"/api/backup/restore", HTTP_POST, guarded, (void*)hBackupRestore},
      {"/setup", HTTP_GET, hSetup, nullptr},
      {"/api/wifi/scan", HTTP_GET, hWifiScan, nullptr},
      {"/api/wifi", HTTP_POST, guarded, (void*)hWifiSave},
      {"/api/wifi/status", HTTP_GET, hWifiStatus, nullptr},
      {"/api/hotspot", HTTP_POST, guarded, (void*)hHotspot},
      {"/api/ota", HTTP_POST, guarded, (void*)hOta},
      {"/api/chat", HTTP_POST, guarded, (void*)hChat},
      {"/api/screen", HTTP_GET, hScreen, nullptr},
  };
  for (auto& u : uris) httpd_register_uri_handler(srv, &u);
  httpd_register_err_handler(srv, HTTPD_404_NOT_FOUND, hNotFound);

  if (mdns_init() == ESP_OK) {
    mdns_hostname_set("pixel");
    mdns_instance_name_set("Pixel Augen");
    mdns_service_add(nullptr, "_http", "_tcp", 80, nullptr, 0);
  }
  ESP_LOGI(TAG, "web interface on http://pixel.local");
}

void setStatus(const char* json) {
  xSemaphoreTake(stMtx, portMAX_DELAY);
  snprintf(statusJson, sizeof(statusJson), "%s", json);
  xSemaphoreGive(stMtx);
}

bool popCommand(char* name, int n) {
  char c[40];
  if (!cmdQ || xQueueReceive(cmdQ, c, 0) != pdTRUE) return false;
  snprintf(name, n, "%s", c);
  return true;
}

}  // namespace web
