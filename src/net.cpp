// WiFi station (reconnects by itself) + SNTP wall clock, and the setup: without a known network
// (or when asked) the robot opens its own access point with a captive portal, the display shows
// a QR code to join it, and the setup page stores the network (and the OpenAI key).
#include "net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <algorithm>

#include "creds.h"
#include "esp_attr.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "nvs_flash.h"

namespace net {

static const char* TAG = "net";
static volatile bool online = false;
static char ipStr[20] = "-";
static char staSsid[33] = "", staPass[65] = "";
static char hsSsid[33] = "", hsPass[65] = "";  // hotspot mode network
static bool hsOn = false;
static volatile bool setupOn = false;
static char apSsid[33] = "", apPass[12] = "";
static volatile int joinState = 0;  // setup page: 0 idle, 1 connecting, 2 connected, 3 failed
static volatile int64_t joinStart = 0, connectedAt = 0, apOpenedAt = 0;

// ------------------------------------------------------------------ captive portal DNS
// every name resolves to the robot, so phones open the setup page by themselves
static void dnsTask(void*) {
  int s = socket(AF_INET, SOCK_DGRAM, 0);
  sockaddr_in a = {};
  a.sin_family = AF_INET;
  a.sin_port = htons(53);
  bind(s, (sockaddr*)&a, sizeof(a));
  EXT_RAM_BSS_ATTR static uint8_t b[512];
  for (;;) {
    sockaddr_in from;
    socklen_t fl = sizeof(from);
    int n = recvfrom(s, b, sizeof(b) - 16, 0, (sockaddr*)&from, &fl);
    if (n < 12 || !setupOn) continue;
    n = net::dnsQuestionEnd(b, n);  // drop what follows the question (EDNS0): the answer goes right after it
    if (n < 0) continue;
    b[2] = 0x81;  // response, recursion desired
    b[3] = 0x80;  // recursion available, no error
    b[4] = 0;
    b[5] = 1;     // one question
    b[6] = 0;
    b[7] = 1;     // one answer
    b[8] = b[9] = b[10] = b[11] = 0;
    static const uint8_t ans[16] = {0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 30, 0, 4, 192, 168, 4, 1};
    memcpy(b + n, ans, 16);
    sendto(s, b, n + 16, 0, (sockaddr*)&from, fl);
  }
}

static void startAp() {
  if (setupOn) return;
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
  snprintf(apSsid, sizeof(apSsid), "Pixel-Setup-%02X%02X", mac[4], mac[5]);
  static const char ABC[] = "abcdefghjkmnpqrstuvwxyz23456789";  // nothing to confuse when typing it
  for (int i = 0; i < 8; ++i) apPass[i] = ABC[esp_random() % (sizeof(ABC) - 1)];
  apPass[8] = 0;
  wifi_config_t ac = {};
  memcpy(ac.ap.ssid, apSsid, strlen(apSsid));
  ac.ap.ssid_len = strlen(apSsid);
  memcpy(ac.ap.password, apPass, 9);
  ac.ap.authmode = WIFI_AUTH_WPA2_PSK;
  ac.ap.max_connection = 3;
  ac.ap.channel = 1;
  esp_wifi_set_mode(WIFI_MODE_APSTA);
  esp_wifi_set_config(WIFI_IF_AP, &ac);
  joinState = 0;
  apOpenedAt = esp_timer_get_time();
  setupOn = true;
  static bool dns = false;
  if (!dns) {
    dns = true;
    xTaskCreatePinnedToCoreWithCaps(dnsTask, "dns", 3072, nullptr, 3, nullptr, 0, MALLOC_CAP_SPIRAM);
  }
  ESP_LOGW(TAG, "setup: access point \"%s\" open, page http://192.168.4.1", apSsid);
}

static void stopAp() {
  if (!setupOn) return;
  setupOn = false;
  esp_wifi_set_mode(WIFI_MODE_STA);
  ESP_LOGI(TAG, "setup finished, access point closed");
}

// ------------------------------------------------------------------ WiFi
static void onEvent(void*, esp_event_base_t base, int32_t id, void* data) {
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
    if (staSsid[0]) esp_wifi_connect();
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    if (online) ESP_LOGW(TAG, "WiFi lost, reconnecting");
    online = false;
    // in the setup a failing network must not keep the radio hopping (the phone would lose the AP)
    if (!staSsid[0] || (setupOn && joinState != 1)) return;
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_wifi_connect();
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    auto* e = (ip_event_got_ip_t*)data;
    ESP_LOGI(TAG, "WiFi connected, IP " IPSTR, IP2STR(&e->ip_info.ip));
    snprintf(ipStr, sizeof(ipStr), IPSTR, IP2STR(&e->ip_info.ip));
    online = true;
    connectedAt = esp_timer_get_time();
    if (joinState == 1) joinState = 2;
    static bool sntp = false;
    if (!sntp) {  // wall clock for the conversation log
      sntp = true;
      setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
      tzset();
      esp_sntp_config_t sc = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
      esp_netif_sntp_init(&sc);
    }
  }
}

static void applySta() {
  wifi_config_t wc = {};
  memcpy(wc.sta.ssid, staSsid, strnlen(staSsid, sizeof(wc.sta.ssid)));  // 32 / 64 bytes, no terminator needed
  memcpy(wc.sta.password, staPass, strnlen(staPass, sizeof(wc.sta.password)));
  wc.sta.threshold.authmode = staPass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
  esp_wifi_set_config(WIFI_IF_STA, &wc);
}

// The setup only opens when no network is configured at all. A network that does not work within 20 s
// is reported. The AP closes 2 min after a successful setup (the page shows the new address), or 10 min
// after it was opened while the robot is online anyway. Meanwhile the known network is retried every
// minute (e.g. the router was still starting after a power cut).
static void watchTask(void*) {
  int64_t retry = 0;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    int64_t now = esp_timer_get_time();
    if (setupOn && joinState == 1 && !online && now - joinStart > 20000000) {
      joinState = 3;
      esp_wifi_disconnect();
    }
    if (setupOn && online && joinState == 2 && now - connectedAt > 120000000) stopAp();
    if (setupOn && online && joinState != 1 && now - apOpenedAt > 600000000) stopAp();
    if (setupOn && !online && joinState != 1 && staSsid[0] && now > retry) {
      retry = now + 60000000;
      esp_wifi_connect();
    }
  }
}

static void wifiStart() {
  esp_err_t r = nvs_flash_init();
  if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    nvs_flash_erase();
    nvs_flash_init();
  }
  nvs_handle_t h;  // own namespace: the board's factory firmware left its own "wifi" entries behind
  if (nvs_open("pixel_wifi", NVS_READONLY, &h) == ESP_OK) {
    size_t n = sizeof(staSsid);
    nvs_get_str(h, "ssid", staSsid, &n);
    n = sizeof(staPass);
    nvs_get_str(h, "pass", staPass, &n);
    n = sizeof(hsSsid);
    nvs_get_str(h, "hs_ssid", hsSsid, &n);
    n = sizeof(hsPass);
    nvs_get_str(h, "hs_pass", hsPass, &n);
    uint8_t on = 0;
    nvs_get_u8(h, "hs_on", &on);
    hsOn = on;
    nvs_close(h);
  }
  if (!staSsid[0] && WIFI_SSID[0]) {  // nothing from the setup: the network built into the firmware
    snprintf(staSsid, sizeof(staSsid), "%s", WIFI_SSID);
    snprintf(staPass, sizeof(staPass), "%s", WIFI_PASS);
    // kept like one from the setup: an update from GitHub (built without secrets.h) still knows it
    if (nvs_open("pixel_wifi", NVS_READWRITE, &h) == ESP_OK) {
      nvs_set_str(h, "ssid", staSsid);
      nvs_set_str(h, "pass", staPass);
      nvs_commit(h);
      nvs_close(h);
    }
  }
  hsOn = hsOn && hsSsid[0];
  if (hsOn) {  // the home network stays stored, only this start uses the hotspot
    memcpy(staSsid, hsSsid, sizeof(staSsid));
    memcpy(staPass, hsPass, sizeof(staPass));
    ESP_LOGW(TAG, "hotspot mode");
  }
  esp_netif_init();
  esp_event_loop_create_default();
  esp_netif_create_default_wifi_sta();
  esp_netif_create_default_wifi_ap();
  wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
  esp_wifi_init(&ic);
  esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, onEvent, nullptr);
  esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, onEvent, nullptr);
  esp_wifi_set_storage(WIFI_STORAGE_RAM);
  esp_wifi_set_mode(WIFI_MODE_STA);
  applySta();
  if (!staSsid[0]) startAp();
  esp_wifi_start();
  esp_wifi_set_ps(WIFI_PS_NONE);  // low latency
  xTaskCreatePinnedToCoreWithCaps(watchTask, "netwatch", 3072, nullptr, 2, nullptr, 0, MALLOC_CAP_SPIRAM);
  if (staSsid[0]) ESP_LOGI(TAG, "connecting to \"%s\"", staSsid);
}

void start() { wifiStart(); }
bool connected() { return online; }
const char* ip() { return ipStr; }
int rssi() {
  wifi_ap_record_t ap;
  return online && esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
}
const char* ssid() { return staSsid; }

bool setupActive() { return setupOn; }
const char* setupSsid() { return apSsid; }
const char* setupPass() { return apPass; }
int joinStatus() { return joinState; }
void forgetWifi() {
  nvs_handle_t h;
  if (nvs_open("pixel_wifi", NVS_READWRITE, &h) == ESP_OK) {
    nvs_erase_all(h);
    nvs_commit(h);
    nvs_close(h);
  }
}
void stopSetup() {
  if (online) stopAp();
}

static bool valid(const char* ssid, const char* pass) {
  if (!ssid[0] || strlen(ssid) > 32 || strlen(pass) > 64 || (pass[0] && strlen(pass) < 8)) return false;
  return !strpbrk(ssid, "\"\\");  // it goes into JSON (status) as is
}

bool saveWifi(const char* ssid, const char* pass) {
  if (!valid(ssid, pass)) return false;
  nvs_handle_t h;
  if (nvs_open("pixel_wifi", NVS_READWRITE, &h) == ESP_OK) {
    nvs_set_str(h, "ssid", ssid);
    nvs_set_str(h, "pass", pass);
    nvs_commit(h);
    nvs_close(h);
  }
  snprintf(staSsid, sizeof(staSsid), "%s", ssid);
  snprintf(staPass, sizeof(staPass), "%s", pass);
  joinStart = esp_timer_get_time();
  joinState = 1;
  esp_wifi_disconnect();
  applySta();
  esp_wifi_connect();
  ESP_LOGI(TAG, "setup: trying \"%s\"", ssid);
  return true;
}

bool hotspot() { return hsOn; }
const char* hotspotSsid() { return hsSsid; }
bool saveHotspot(const char* ssid, const char* pass) {
  if (ssid[0] && !pass[0] && !strcmp(ssid, hsSsid)) pass = hsPass;  // the page never gets the password back
  if (ssid[0] && !valid(ssid, pass)) return false;
  nvs_handle_t h;
  if (nvs_open("pixel_wifi", NVS_READWRITE, &h) != ESP_OK) return false;
  char p[65];
  snprintf(p, sizeof(p), "%s", pass);
  snprintf(hsSsid, sizeof(hsSsid), "%s", ssid);
  snprintf(hsPass, sizeof(hsPass), "%s", p);
  nvs_set_str(h, "hs_ssid", hsSsid);
  nvs_set_str(h, "hs_pass", hsPass);
  if (!ssid[0]) nvs_set_u8(h, "hs_on", 0);
  nvs_commit(h);
  nvs_close(h);
  return true;
}
void setHotspot(bool on) {
  nvs_handle_t h;
  if (nvs_open("pixel_wifi", NVS_READWRITE, &h) != ESP_OK) return;
  nvs_set_u8(h, "hs_on", on);
  nvs_commit(h);
  nvs_close(h);
}

int scan(char* out, int cap) {
  wifi_scan_config_t sc = {};
  uint16_t n = 24;
  EXT_RAM_BSS_ATTR static wifi_ap_record_t rec[24];
  if (esp_wifi_scan_start(&sc, true) != ESP_OK || esp_wifi_scan_get_ap_records(&n, rec) != ESP_OK) n = 0;
  std::sort(rec, rec + n, [](const wifi_ap_record_t& a, const wifi_ap_record_t& b) { return a.rssi > b.rssi; });
  int len = snprintf(out, cap, "[");
  int cnt = 0;
  for (int i = 0; i < n; ++i) {
    const char* s = (const char*)rec[i].ssid;
    bool skip = !s[0] || strchr(s, '"') || strchr(s, '\\');  // hidden, or not safe to put into JSON as is
    for (int k = 0; k < i && !skip; ++k) skip = !strcmp(s, (const char*)rec[k].ssid);
    if (skip || len > cap - 80) continue;
    len += snprintf(out + len, cap - len, "%s{\"ssid\":\"%s\",\"rssi\":%d,\"open\":%s}", cnt++ ? "," : "", s,
                    rec[i].rssi, rec[i].authmode == WIFI_AUTH_OPEN ? "true" : "false");
  }
  snprintf(out + len, cap - len, "]");
  return cnt;
}

}  // namespace net
