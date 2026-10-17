#include "update.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "screen.h"
#include "settings.h"
#include "version.h"

namespace update {

static const char* TAG = "update";
static volatile State st = IDLE;
static volatile int prog = 0;
static volatile bool busy = false, autoInstall = false, installReq = false, psramStack = false;
static char ver[24] = "", url[512] = "", notes[600] = "", err[96] = "";

// the latest release: its version, notes and the update file
static bool fetchLatest() {
  char api[128];
  snprintf(api, sizeof(api), "https://api.github.com/repos/%s/releases/latest", PIXEL_REPO);
  esp_http_client_config_t c = {};
  c.url = api;
  c.crt_bundle_attach = esp_crt_bundle_attach;
  c.timeout_ms = 15000;
  c.buffer_size = 2048;
  c.user_agent = "pixel-buddy/" PIXEL_VERSION;
  esp_http_client_handle_t h = esp_http_client_init(&c);
  esp_http_client_set_header(h, "Accept", "application/vnd.github+json");
  bool ok = false;
  err[0] = 0;
  if (esp_http_client_open(h, 0) == ESP_OK) {
    esp_http_client_fetch_headers(h);
    int code = esp_http_client_get_status_code(h);
    const int CAP = 128 * 1024;
    char* b = (char*)heap_caps_malloc(CAP, MALLOC_CAP_SPIRAM);
    int n = 0, r;
    while (b && n < CAP - 1 && (r = esp_http_client_read(h, b + n, CAP - 1 - n)) > 0) {
      n += r;
      if (esp_http_client_is_complete_data_received(h)) break;  // do not wait for the connection to close
    }
    if (b) b[n] = 0;
    cJSON* j = code == 200 && b ? cJSON_Parse(b) : nullptr;
    const cJSON* tag = cJSON_GetObjectItem(j, "tag_name");
    const cJSON* body = cJSON_GetObjectItem(j, "body");
    const cJSON* a;
    url[0] = 0;
    cJSON_ArrayForEach(a, cJSON_GetObjectItem(j, "assets")) {
      const cJSON* nm = cJSON_GetObjectItem(a, "name");
      const cJSON* u = cJSON_GetObjectItem(a, "browser_download_url");
      if (cJSON_IsString(nm) && !strcmp(nm->valuestring, "pixel-update.bin") && cJSON_IsString(u))
        snprintf(url, sizeof(url), "%s", u->valuestring);
    }
    if (cJSON_IsString(tag)) snprintf(ver, sizeof(ver), "%s", tag->valuestring + (tag->valuestring[0] == 'v'));
    snprintf(notes, sizeof(notes), "%s", cJSON_IsString(body) ? body->valuestring : "");
    ok = cJSON_IsString(tag) && url[0];
    if (code == 404)
      snprintf(err, sizeof(err), "noch keine Version auf GitHub");
    else if (code != 200)
      snprintf(err, sizeof(err), "GitHub antwortet nicht (HTTP %d)", code);
    else if (!ok)
      snprintf(err, sizeof(err), "die neueste Version hat keine pixel-update.bin");
    if (j) cJSON_Delete(j);
    free(b);
  } else {
    snprintf(err, sizeof(err), "keine Verbindung zu GitHub");
  }
  esp_http_client_cleanup(h);
  return ok;
}

static void splash(const char* line, int pct) {
  char t[32];
  snprintf(t, sizeof(t), "Update %s", ver);
  screen::splash(t, line, pct);
}

// download into the other app slot; the bootloader falls back to this firmware if the new one does not start
static bool doInstall() {
  st = DOWNLOADING;
  prog = 0;
  const esp_app_desc_t* d = esp_app_get_description();
  char fw[48];
  snprintf(fw, sizeof(fw), "%s (%s %s)", PIXEL_VERSION, d->date, d->time);
  settings::backup(fw);  // the settings as they were, in case the new version loses them
  splash("Bitte nicht ausschalten", 0);
  ESP_LOGI(TAG, "installing %s from %s", ver, url);
  esp_http_client_config_t c = {};
  c.url = url;
  c.crt_bundle_attach = esp_crt_bundle_attach;
  c.timeout_ms = 20000;
  c.buffer_size = 4096;
  c.buffer_size_tx = 2048;  // the redirect to GitHub's file storage has a long address
  c.keep_alive_enable = true;
  c.user_agent = "pixel-buddy/" PIXEL_VERSION;
  esp_https_ota_config_t oc = {};
  oc.http_config = &c;
  esp_https_ota_handle_t oh = nullptr;
  esp_err_t e = esp_https_ota_begin(&oc, &oh);
  if (e == ESP_OK) {
    int total = esp_https_ota_get_image_size(oh), shown = -1;
    while ((e = esp_https_ota_perform(oh)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
      int rd = esp_https_ota_get_image_len_read(oh);
      prog = total > 0 ? (int)((long long)rd * 100 / total) : 0;
      if (prog / 2 != shown) {
        shown = prog / 2;
        splash("Bitte nicht ausschalten", prog);
      }
    }
    if (e == ESP_OK && esp_https_ota_is_complete_data_received(oh))
      e = esp_https_ota_finish(oh);
    else
      esp_https_ota_abort(oh);
  }
  if (e != ESP_OK) {
    snprintf(err, sizeof(err), "Download fehlgeschlagen (%s)", esp_err_to_name(e));
    ESP_LOGW(TAG, "%s", err);
    screen::hide();
    st = FAILED;
    return false;
  }
  st = DONE;
  prog = 100;
  splash("Fertig - starte neu", 100);
  ESP_LOGI(TAG, "%s installed, restarting", ver);
  vTaskDelay(pdMS_TO_TICKS(1500));
  esp_restart();
  return true;
}

// flash writes need an internal-RAM stack, so a short-lived task of its own
static void task(void*) {
  if (!installReq) {
    st = CHECKING;
    bool ok = fetchLatest();
    st = !ok ? FAILED : compare(ver, PIXEL_VERSION) > 0 ? AVAILABLE : UP_TO_DATE;
    ESP_LOGI(TAG, "latest on GitHub: %s (this one %s)%s%s", ver[0] ? ver : "?", PIXEL_VERSION, err[0] ? " - " : "", err);
  }
  if (st == AVAILABLE && (autoInstall || installReq)) doInstall();
  ESP_LOGI(TAG, "stack left: %u bytes", (unsigned)uxTaskGetStackHighWaterMark(nullptr));
  installReq = false;
  autoInstall = false;
  busy = false;
  if (psramStack)
    vTaskDeleteWithCaps(nullptr);
  else
    vTaskDelete(nullptr);
}
static void start() {
  if (busy) return;
  busy = true;
  // priority 4 like the other TLS helper: below it the handshake (software crypto) starves on core 0.
  // Installing writes the flash: that needs a stack in internal RAM. Only looking does not: PSRAM.
  psramStack = !(installReq || autoInstall);
  BaseType_t ok = installReq || autoInstall
                      ? xTaskCreatePinnedToCore(task, "update", 6144, nullptr, 4, nullptr, 0)
                      : xTaskCreatePinnedToCoreWithCaps(task, "update", 6144, nullptr, 4, nullptr, 0, MALLOC_CAP_SPIRAM);
  if (ok != pdPASS) {
    snprintf(err, sizeof(err), "zu wenig freier Speicher (%u Bytes am Stück) - nach einem Neustart klappt es",
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    ESP_LOGW(TAG, "%s", err);
    st = FAILED;
    busy = false;
  }
}
void check(bool inst) {
  autoInstall = inst;
  start();
}
void install() {
  if (st != AVAILABLE) return;
  installReq = true;
  start();
}
State state() { return st; }
int progress() { return prog; }
const char* latest() { return ver; }

void json(char* out, int cap) {
  static const char* N[] = {"idle", "checking", "up_to_date", "available", "downloading", "done", "failed"};
  cJSON* o = cJSON_CreateObject();
  cJSON_AddStringToObject(o, "current", PIXEL_VERSION);
  cJSON_AddStringToObject(o, "latest", ver);
  cJSON_AddStringToObject(o, "state", N[st]);
  cJSON_AddNumberToObject(o, "progress", prog);
  cJSON_AddStringToObject(o, "notes", notes);
  cJSON_AddStringToObject(o, "error", err);
  cJSON_AddStringToObject(o, "repo", PIXEL_REPO);
  char* t = cJSON_PrintUnformatted(o);
  cJSON_Delete(o);
  snprintf(out, cap, "%s", t ? t : "{}");
  cJSON_free(t);
}

}  // namespace update
