#include "update.h"
#include "i18n.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "live.h"
#include "mic.h"
#include "models_sha.h"
#include "psa/crypto.h"
#include "screen.h"
#include "settings.h"
#include "version.h"

namespace update {

static const char* TAG = "update";
static volatile State st = IDLE;
static volatile int prog = 0;
static volatile bool busy = false, autoInstall = false, installReq = false;
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
      snprintf(err, sizeof(err), "no release on GitHub yet");
    else if (code != 200)
      snprintf(err, sizeof(err), "GitHub does not answer (HTTP %d)", code);
    else if (!ok)
      snprintf(err, sizeof(err), "the latest release has no pixel-update.bin");
    if (j) cJSON_Delete(j);
    free(b);
  } else {
    snprintf(err, sizeof(err), "no connection to GitHub");
  }
  esp_http_client_cleanup(h);
  return ok;
}

static void splash(const char* line, int pct) {
  char t[32];
  snprintf(t, sizeof(t), i18n::t(i18n::T_UPDATE), ver);
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
  splash(i18n::t(i18n::T_UPDATE_KEEP_ON), 0);
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
        splash(i18n::t(i18n::T_UPDATE_KEEP_ON), prog);
      }
    }
    if (e == ESP_OK && esp_https_ota_is_complete_data_received(oh))
      e = esp_https_ota_finish(oh);
    else
      esp_https_ota_abort(oh);
  }
  if (e != ESP_OK) {
    snprintf(err, sizeof(err), "download failed (%s)", esp_err_to_name(e));
    ESP_LOGW(TAG, "%s", err);
    screen::hide();
    st = FAILED;
    return false;
  }
  st = DONE;
  prog = 100;
  splash(i18n::t(i18n::T_UPDATE_DONE), 100);
  ESP_LOGI(TAG, "%s installed, restarting", ver);
  vTaskDelay(pdMS_TO_TICKS(1500));
  esp_restart();
  return true;
}

// Looking runs in a task with its stack in PSRAM. Installing writes the flash, that needs a stack in internal
// RAM: the warm conversation connections are closed first to make room for it.
static void installTask(void*) {
  doInstall();  // restarts the robot when it worked
  busy = false;
  vTaskDelete(nullptr);
}
static void spawnInstall() {
  live::rest();
  for (int k = 0; k < 40 && heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) < 8192; ++k) vTaskDelay(pdMS_TO_TICKS(100));
  if (xTaskCreatePinnedToCore(installTask, "update_ota", 6144, nullptr, 4, nullptr, 0) == pdPASS) return;
  snprintf(err, sizeof(err), "not enough free memory (%u bytes in one piece) - it works after a restart",
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  ESP_LOGW(TAG, "%s", err);
  st = FAILED;
  busy = false;
}
static void task(void*) {
  if (!installReq) {
    st = CHECKING;
    bool ok = fetchLatest();
    st = !ok ? FAILED : compare(ver, PIXEL_VERSION) > 0 ? AVAILABLE : UP_TO_DATE;
    ESP_LOGI(TAG, "latest on GitHub: %s (this one %s)%s%s", ver[0] ? ver : "?", PIXEL_VERSION, err[0] ? " - " : "", err);
  }
  bool inst = st == AVAILABLE && (autoInstall || installReq);
  installReq = false;
  autoInstall = false;
  if (inst)
    spawnInstall();  // busy stays set until the install task is through
  else
    busy = false;
  vTaskDeleteWithCaps(nullptr);
}
static void start() {
  if (busy) return;
  busy = true;
  // priority 4 like the other TLS helper: below it the handshake (software crypto) starves on core 0
  if (xTaskCreatePinnedToCoreWithCaps(task, "update", 6144, nullptr, 4, nullptr, 0, MALLOC_CAP_SPIRAM) != pdPASS) {
    snprintf(err, sizeof(err), "could not start");
    st = FAILED;
    busy = false;
  }
}
// ---- the wake word models ("model" partition). An update over WiFi only replaces the firmware: when the models
// on the flash are not the ones this firmware was built with, it fetches them from its own release
// (pixel-models.bin) and restarts. The first sector (the list of models) is written last, after the download
// checked out: until then esp-sr finds no models at all instead of broken ones.
static bool modelsMatch(const esp_partition_t* p) {
  const uint8_t* m;
  esp_partition_mmap_handle_t mh;
  if (esp_partition_mmap(p, 0, MODELS_SIZE, ESP_PARTITION_MMAP_DATA, (const void**)&m, &mh) != ESP_OK) return true;
  psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
  psa_hash_setup(&op, PSA_ALG_SHA_256);
  uint8_t b[1024], d[32];  // through RAM: the SHA unit cannot read the flash mapping directly
  for (long o = 0; o < MODELS_SIZE; o += sizeof(b)) {
    long n = MODELS_SIZE - o < (long)sizeof(b) ? MODELS_SIZE - o : sizeof(b);
    memcpy(b, m + o, n);
    psa_hash_update(&op, b, n);
  }
  size_t dn;
  bool same = psa_hash_finish(&op, d, sizeof(d), &dn) == PSA_SUCCESS && !memcmp(d, MODELS_SHA, 32);
  esp_partition_munmap(mh);
  return same;
}
static bool modelsFetch(const esp_partition_t* p) {
  char u[160], title[32];
  snprintf(u, sizeof(u), "https://github.com/%s/releases/download/v%s/pixel-models.bin", PIXEL_REPO, PIXEL_VERSION);
  snprintf(title, sizeof(title), i18n::t(i18n::T_UPDATE), PIXEL_VERSION);
  live::rest();  // room for TLS
  esp_http_client_config_t c = {};
  c.url = u;
  c.crt_bundle_attach = esp_crt_bundle_attach;
  c.timeout_ms = 20000;
  c.buffer_size = 4096;
  c.buffer_size_tx = 2048;  // the redirect to GitHub's file storage has a long address
  c.user_agent = "pixel-buddy/" PIXEL_VERSION;
  esp_http_client_handle_t h = esp_http_client_init(&c);
  int code = 0;
  long len = -1;
  for (int k = 0; k < 4; ++k) {  // follow the redirects by hand (open/read does not)
    if (esp_http_client_open(h, 0) != ESP_OK) {
      code = 0;
      break;
    }
    len = esp_http_client_fetch_headers(h);
    code = esp_http_client_get_status_code(h);
    if (code < 300 || code > 399) break;
    esp_http_client_set_redirection(h);
    esp_http_client_close(h);
  }
  bool ok = code == 200 && len == MODELS_SIZE;
  uint8_t* buf = ok ? (uint8_t*)heap_caps_malloc(4096, MALLOC_CAP_INTERNAL) : nullptr;
  uint8_t* first = ok ? (uint8_t*)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM) : nullptr;
  if (!buf || !first) {
    if (ok) snprintf(err, sizeof(err), "models: no memory");
    else snprintf(err, sizeof(err), "models: HTTP %d, %ld bytes (want %d)", code, len, MODELS_SIZE);
    ok = false;
  } else {
    ESP_LOGI(TAG, "fetching the wake word models (%d bytes)", MODELS_SIZE);
    mic::wakeStop();  // WakeNet reads its model straight from this partition
    screen::splash(title, i18n::t(i18n::T_UPDATE_KEEP_ON), 0);
    psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
    psa_hash_setup(&op, PSA_ALG_SHA_256);
    long off = 0;
    int fill = 0, shown = 0;
    ok = esp_partition_erase_range(p, 0, 4096) == ESP_OK;  // from now on: no models until the end
    while (ok && off < MODELS_SIZE) {
      int r = esp_http_client_read(h, (char*)buf + fill, 4096 - fill);
      if (r <= 0) {
        ok = false;
        break;
      }
      fill += r;
      if (fill < 4096 && off + fill < MODELS_SIZE) continue;
      psa_hash_update(&op, buf, fill);
      memset(buf + fill, 0xFF, 4096 - fill);
      if (off == 0)
        memcpy(first, buf, 4096);
      else
        ok = esp_partition_erase_range(p, off, 4096) == ESP_OK && esp_partition_write(p, off, buf, 4096) == ESP_OK;
      off += fill;
      fill = 0;
      if (off * 50 / MODELS_SIZE != shown) {
        shown = off * 50 / MODELS_SIZE;
        screen::splash(title, i18n::t(i18n::T_UPDATE_KEEP_ON), shown * 2);
      }
    }
    uint8_t d[32];
    size_t dn;
    ok = psa_hash_finish(&op, d, sizeof(d), &dn) == PSA_SUCCESS && ok && !memcmp(d, MODELS_SHA, 32);
    if (ok) {
      memcpy(buf, first, 4096);
      ok = esp_partition_write(p, 0, buf, 4096) == ESP_OK;
    }
    if (!ok) snprintf(err, sizeof(err), "models: download failed");
  }
  free(buf);
  free(first);
  esp_http_client_cleanup(h);
  if (!ok) {
    ESP_LOGW(TAG, "%s", err);
    screen::hide();
    return false;
  }
  ESP_LOGI(TAG, "wake word models updated, restarting");
  screen::splash(title, i18n::t(i18n::T_UPDATE_DONE), 100);
  vTaskDelay(pdMS_TO_TICKS(1500));
  esp_restart();
  return true;
}
static void modelsTask(void*) {
  const esp_partition_t* p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "model");
  if (p && MODELS_SIZE > 0 && MODELS_SIZE <= (long)p->size && !modelsMatch(p)) modelsFetch(p);
  busy = false;
  vTaskDelete(nullptr);
}
void models() {
  static bool done = false;
  if (done || busy) return;
  done = true;
  busy = true;
  // writes the flash: a stack in internal RAM
  if (xTaskCreatePinnedToCore(modelsTask, "update_models", 6144, nullptr, 4, nullptr, 0) != pdPASS) busy = false;
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
