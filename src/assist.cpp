#include "assist.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <initializer_list>

#include "cJSON.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sd.h"

namespace assist {

static const char* TAG = "assist";
static const char* TIMERS = "/sdcard/pixel/timers.json";
static const char* MEMORY = "/sdcard/pixel/memory.txt";
static const char* USAGE = "/sdcard/pixel/usage.json";
static SemaphoreHandle_t mtx;

// ------------------------------------------------------------------ helpers
static long readFile(const char* path, char* dst, long cap) {
  dst[0] = 0;
  if (!sd::ok()) return 0;
  FILE* f = fopen(path, "r");
  if (!f) return 0;
  long n = fread(dst, 1, cap - 1, f);
  fclose(f);
  dst[n < 0 ? 0 : n] = 0;
  return n;
}
static void writeFile(const char* path, const char* text) { sd::writeLater(path, text); }
static bool clockSet() { return time(nullptr) > 1700000000; }  // SNTP has run

// ------------------------------------------------------------------ timers
struct Timer {
  time_t due;
  char label[48];
};
static Timer tm_[8];
static int tmN = 0;

static void saveTimers() {
  cJSON* a = cJSON_CreateArray();
  for (int i = 0; i < tmN; ++i) {
    cJSON* o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "due", (double)tm_[i].due);
    cJSON_AddStringToObject(o, "label", tm_[i].label);
    cJSON_AddItemToArray(a, o);
  }
  char* t = cJSON_PrintUnformatted(a);
  cJSON_Delete(a);
  writeFile(TIMERS, t);
  cJSON_free(t);
}
static bool add(time_t due, const char* label) {
  xSemaphoreTake(mtx, portMAX_DELAY);
  bool ok = tmN < 8;
  if (ok) {
    tm_[tmN].due = due;
    snprintf(tm_[tmN].label, sizeof(tm_[0].label), "%s", label ? label : "");
    tmN++;
    saveTimers();
  }
  xSemaphoreGive(mtx);
  return ok;
}
bool addTimer(int seconds, const char* label) {
  if (seconds < 1 || seconds > 7 * 86400 || !clockSet()) return false;
  return add(time(nullptr) + seconds, label);
}
bool addAlarm(int hour, int minute, const char* label) {
  if (hour < 0 || hour > 23 || minute < 0 || minute > 59 || !clockSet()) return false;
  return add(nextAlarm(time(nullptr), hour, minute), label);
}
static bool contains(const char* hay, const char* needle) {  // case-insensitive (ASCII)
  for (; *hay; ++hay) {
    int i = 0;
    while (needle[i] && hay[i] && tolower((unsigned char)hay[i]) == tolower((unsigned char)needle[i])) ++i;
    if (!needle[i]) return true;
  }
  return !needle[0];
}
int cancel(const char* label) {
  bool all = !label || !label[0] || contains(label, "alle");
  xSemaphoreTake(mtx, portMAX_DELAY);
  int k = 0, removed = 0;
  for (int i = 0; i < tmN; ++i) {
    if (all || contains(tm_[i].label, label))
      removed++;
    else
      tm_[k++] = tm_[i];
  }
  tmN = k;
  saveTimers();
  xSemaphoreGive(mtx);
  return removed;
}
void listTimers(char* out, int cap) {
  xSemaphoreTake(mtx, portMAX_DELAY);
  int n = snprintf(out, cap, tmN ? "" : "Keine Timer oder Wecker gestellt.");
  time_t now = time(nullptr);
  for (int i = 0; i < tmN && n < cap; ++i) {
    struct tm t;
    localtime_r(&tm_[i].due, &t);
    long left = (long)(tm_[i].due - now);
    n += snprintf(out + n, cap - n, "%s um %02d:%02d (in %ld min). ", tm_[i].label[0] ? tm_[i].label : "Timer",
                  t.tm_hour, t.tm_min, left > 0 ? (left + 59) / 60 : 0);
  }
  xSemaphoreGive(mtx);
}
bool popDue(char* label, int cap) {
  if (!tmN || !clockSet()) return false;
  time_t now = time(nullptr);
  bool hit = false;
  xSemaphoreTake(mtx, portMAX_DELAY);
  for (int i = 0; i < tmN; ++i)
    if (tm_[i].due <= now) {
      snprintf(label, cap, "%s", tm_[i].label);
      tm_[i] = tm_[--tmN];
      saveTimers();
      hit = true;
      break;
    }
  xSemaphoreGive(mtx);
  return hit;
}
int timerCount() { return tmN; }

// ------------------------------------------------------------------ memory
EXT_RAM_BSS_ATTR static char mem[2048];

bool remember(const char* fact) {
  xSemaphoreTake(mtx, portMAX_DELAY);
  int n = strlen(mem);
  bool fits = n + strlen(fact) + 4 < sizeof(mem);
  if (fits) {
    snprintf(mem + n, sizeof(mem) - n, "- %s\n", fact);
    writeFile(MEMORY, mem);
  }
  xSemaphoreGive(mtx);
  return fits;
}
int forget(const char* what) {
  xSemaphoreTake(mtx, portMAX_DELAY);
  EXT_RAM_BSS_ATTR static char out[sizeof(mem)], one[sizeof(mem)];
  int k = 0, removed = 0;
  for (const char* line = mem; *line;) {
    const char* end = strchr(line, '\n');
    int len = end ? (int)(end - line + 1) : (int)strlen(line);
    snprintf(one, sizeof(one), "%.*s", len, line);
    if (!what[0] || contains(one, what)) {
      removed++;
    } else {
      memcpy(out + k, line, len);
      k += len;
    }
    line += len;
  }
  out[k] = 0;
  memcpy(mem, out, k + 1);
  writeFile(MEMORY, mem);
  xSemaphoreGive(mtx);
  return removed;
}
const char* memory() { return mem; }

// ------------------------------------------------------------------ usage
static double usdDay = 0, usdMonth = 0;
static char dayKey[40] = "", monthKey[40] = "";
static void keys(char* d, char* m) {  // 40 bytes each
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  snprintf(d, 40, "%04d-%02d-%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
  snprintf(m, 40, "%04d-%02d", t.tm_year + 1900, t.tm_mon + 1);
}
static void roll() {  // a new day / month starts from zero
  char d[40], m[40];
  keys(d, m);
  if (strcmp(d, dayKey)) {
    usdDay = 0;
    snprintf(dayKey, sizeof(dayKey), "%s", d);
  }
  if (strcmp(m, monthKey)) {
    usdMonth = 0;
    snprintf(monthKey, sizeof(monthKey), "%s", m);
  }
}
void addCost(double usd) {
  if (usd <= 0) return;
  xSemaphoreTake(mtx, portMAX_DELAY);
  roll();
  usdDay += usd;
  usdMonth += usd;
  char t[200];
  snprintf(t, sizeof(t), "{\"day\":\"%s\",\"today\":%.5f,\"month\":\"%s\",\"month_usd\":%.5f}", dayKey, usdDay, monthKey,
           usdMonth);
  writeFile(USAGE, t);
  xSemaphoreGive(mtx);
}
double today() {
  if (clockSet()) roll();
  return usdDay;
}
double month() {
  if (clockSet()) roll();
  return usdMonth;
}
bool overBudget(double perDay) { return perDay > 0 && today() >= perDay; }

void wipe() {
  xSemaphoreTake(mtx, portMAX_DELAY);
  tmN = 0;
  mem[0] = 0;
  usdDay = usdMonth = 0;
  for (const char* f : {TIMERS, MEMORY, USAGE}) remove(f);
  xSemaphoreGive(mtx);
}

// ------------------------------------------------------------------ init
void init() {
  mtx = xSemaphoreCreateMutex();
  EXT_RAM_BSS_ATTR static char buf[2048];
  if (readFile(TIMERS, buf, sizeof(buf))) {
    cJSON* a = cJSON_Parse(buf);
    const cJSON* o;
    cJSON_ArrayForEach(o, a) {
      const cJSON* d = cJSON_GetObjectItem(o, "due");
      const cJSON* l = cJSON_GetObjectItem(o, "label");
      if (cJSON_IsNumber(d) && tmN < 8) {
        tm_[tmN].due = (time_t)d->valuedouble;
        snprintf(tm_[tmN].label, sizeof(tm_[0].label), "%s", cJSON_IsString(l) ? l->valuestring : "");
        tmN++;
      }
    }
    if (a) cJSON_Delete(a);
  }
  readFile(MEMORY, mem, sizeof(mem));
  if (readFile(USAGE, buf, sizeof(buf))) {
    cJSON* j = cJSON_Parse(buf);
    const cJSON* d = cJSON_GetObjectItem(j, "day");
    const cJSON* m = cJSON_GetObjectItem(j, "month");
    if (cJSON_IsString(d)) snprintf(dayKey, sizeof(dayKey), "%s", d->valuestring);
    if (cJSON_IsString(m)) snprintf(monthKey, sizeof(monthKey), "%s", m->valuestring);
    const cJSON* a = cJSON_GetObjectItem(j, "today");
    const cJSON* b = cJSON_GetObjectItem(j, "month_usd");
    usdDay = cJSON_IsNumber(a) ? a->valuedouble : 0;
    usdMonth = cJSON_IsNumber(b) ? b->valuedouble : 0;
    if (j) cJSON_Delete(j);
  }
  ESP_LOGI(TAG, "%d timers, %d bytes memory, today %.3f USD", tmN, (int)strlen(mem), usdDay);
}

}  // namespace assist
