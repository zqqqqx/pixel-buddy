#include "sd.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>

#include "driver/sdspi_host.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "esp_heap_caps.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <stdlib.h>
#include "sdmmc_cmd.h"

namespace sd {

static const char* TAG = "sd";
static bool mounted = false;

bool mount() {
  spi_bus_config_t bc = {};
  bc.mosi_io_num = 18;
  bc.miso_io_num = 21;
  bc.sclk_io_num = 17;
  bc.quadwp_io_num = -1;
  bc.quadhd_io_num = -1;
  bc.max_transfer_sz = 4096;
  if (spi_bus_initialize(SPI2_HOST, &bc, SPI_DMA_CH_AUTO) != ESP_OK) return false;
  sdmmc_host_t host = SDSPI_HOST_DEFAULT();
  host.slot = SPI2_HOST;
  sdspi_device_config_t dc = SDSPI_DEVICE_CONFIG_DEFAULT();
  dc.gpio_cs = GPIO_NUM_13;
  dc.host_id = SPI2_HOST;
  esp_vfs_fat_sdmmc_mount_config_t mc = {};
  mc.max_files = 4;
  mc.allocation_unit_size = 16 * 1024;
  sdmmc_card_t* card = nullptr;
  esp_err_t e = ESP_FAIL;
  for (int attempt = 0; attempt < 3 && e != ESP_OK; ++attempt) {  // a card interrupted by a reset may need a retry
    if (attempt) vTaskDelay(pdMS_TO_TICKS(250));
    e = esp_vfs_fat_sdspi_mount("/sdcard", &host, &dc, &mc, &card);
  }
  if (e != ESP_OK) {
    ESP_LOGW(TAG, "no SD card (%s)", esp_err_to_name(e));
    return false;
  }
  mkdir("/sdcard/pixel", 0775);
  mounted = true;
  ESP_LOGI(TAG, "SD card mounted, %llu MB", (unsigned long long)card->csd.capacity * card->csd.sector_size / 1048576);
  return true;
}

bool ok() { return mounted; }

// Writes run in a low-priority task on core 0: a card can be busy for 100+ ms per write, and the
// callers (conversation task, frame task) must not wait for it. One queue keeps the order.
bool saveWav(const char* dir, const char* prefix, const int16_t* pcm, int n, char* name, int cap);
struct Write {
  char path[48];  // file, or for a WAV "dir|prefix"
  int kind;       // 0 append text, 1 replace text, 2 WAV (len samples)
  char* data;     // PSRAM copy
  int len;
};
static QueueHandle_t wq = nullptr;
static void writer(void*) {
  Write w;
  for (;;) {
    xQueueReceive(wq, &w, portMAX_DELAY);
    if (w.kind == 2) {
      char* bar = strchr(w.path, '|');
      *bar = 0;
      saveWav(w.path, bar + 1, (const int16_t*)w.data, w.len, nullptr, 0);
    } else if (FILE* f = fopen(w.path, w.kind ? "w" : "a")) {
      fputs(w.data, f);
      fclose(f);
    }
    free(w.data);
  }
}
static bool queue(Write& w) {
  if (!wq) {
    wq = xQueueCreate(24, sizeof(Write));
    xTaskCreatePinnedToCoreWithCaps(writer, "sd_write", 4096, nullptr, 2, nullptr, 0, MALLOC_CAP_SPIRAM);
  }
  if (xQueueSend(wq, &w, 0) == pdTRUE) return true;
  free(w.data);  // queue full (card gone?): dropped rather than blocking
  return false;
}
bool saveWavLater(const char* dir, const char* prefix, const int16_t* pcm, int n) {
  if (!mounted) return false;
  Write w;
  snprintf(w.path, sizeof(w.path), "%s|%s", dir, prefix);
  w.kind = 2;
  w.len = n;
  w.data = (char*)heap_caps_malloc(n * 2, MALLOC_CAP_SPIRAM);
  if (!w.data) return false;
  memcpy(w.data, pcm, n * 2);
  return queue(w);
}
static bool later(const char* path, const char* text, bool app) {
  if (!mounted) return false;
  Write w;
  snprintf(w.path, sizeof(w.path), "%s", path);
  w.kind = app ? 0 : 1;
  w.len = 0;
  w.data = (char*)heap_caps_malloc(strlen(text) + 1, MALLOC_CAP_SPIRAM);
  if (!w.data) return false;
  strcpy(w.data, text);
  return queue(w);
}
bool append(const char* path, const char* text) { return later(path, text, true); }
bool writeLater(const char* path, const char* text) { return later(path, text, false); }

long readTail(const char* path, char* dst, long cap) {
  dst[0] = 0;
  if (!mounted) return 0;
  FILE* f = fopen(path, "r");
  if (!f) return 0;
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  long start = size > cap - 1 ? size - (cap - 1) : 0;
  fseek(f, start, SEEK_SET);
  long n = fread(dst, 1, cap - 1, f);
  dst[n] = 0;
  fclose(f);
  return n;
}

int countFiles(const char* dir, const char* ext) {
  DIR* d = mounted ? opendir(dir) : nullptr;
  if (!d) return 0;
  int n = 0;
  size_t el = strlen(ext);
  while (struct dirent* e = readdir(d)) {
    size_t l = strlen(e->d_name);
    n += l >= el && !strcmp(e->d_name + l - el, ext);
  }
  closedir(d);
  return n;
}

bool saveWav(const char* dir, const char* prefix, const int16_t* pcm, int n, char* name, int cap) {
  if (!ok()) return false;
  mkdir(dir, 0775);
  char path[80];
  int idx = 0;
  struct stat st;
  do snprintf(path, sizeof(path), "%s/%s_%03d.wav", dir, prefix, idx++);
  while (stat(path, &st) == 0 && idx < 1000);
  FILE* f = fopen(path, "wb");
  if (!f) return false;
  uint8_t h[44];
  auto u32 = [&](int o, uint32_t v) { memcpy(h + o, &v, 4); };
  auto u16 = [&](int o, uint16_t v) { memcpy(h + o, &v, 2); };
  memcpy(h, "RIFF", 4);
  u32(4, 36 + n * 2);
  memcpy(h + 8, "WAVEfmt ", 8);
  u32(16, 16);
  u16(20, 1);
  u16(22, 1);
  u32(24, 16000);
  u32(28, 32000);
  u16(32, 2);
  u16(34, 16);
  memcpy(h + 36, "data", 4);
  u32(40, n * 2);
  bool good = fwrite(h, 1, 44, f) == 44 && fwrite(pcm, 2, n, f) == (size_t)n;
  fclose(f);
  if (name) snprintf(name, cap, "%s", strrchr(path, '/') + 1);
  return good;
}

}  // namespace sd
