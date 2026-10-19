#include "chats.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>

#include "cJSON.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sd.h"

namespace chats {

static const char* TAG = "chats";
static const char* DIR_ = "/sdcard/pixel/chats";
static const int MAXC = 500;  // listed at most (the newest)
static SemaphoreHandle_t mtx = nullptr;
static char cur[16] = "";      // the conversation the lines go to, "" = a new one with the next line

struct Ent {
  char id[16];
  char at[17];     // "2026-10-19 20:15"
  char title[72];
};
EXT_RAM_BSS_ATTR static Ent cache[MAXC];  // what the files say at their start (they only grow at the end)
static int nCache = 0;

static void path(const char* id, char* p, int cap) { snprintf(p, cap, "%s/%.15s.txt", DIR_, id); }

// Reads in 512 byte pieces through a small buffer on the stack: the card's DMA then needs no big block of the
// (scarce) internal RAM, which a conversation's TLS connections use up.
static long readFile(const char* p, char* dst, long cap) {
  FILE* f = fopen(p, "r");
  if (!f) return -1;
  setvbuf(f, nullptr, _IONBF, 0);
  char b[512];
  long n = 0;
  size_t k;
  while (n < cap - 1 && (k = fread(b, 1, sizeof(b), f)) > 0) {
    if ((long)k > cap - 1 - n) k = cap - 1 - n;
    memcpy(dst + n, b, k);
    n += k;
  }
  dst[n] = 0;
  fclose(f);
  return n;
}

// the old single log (chat.old.log, chat.log) into one file per conversation; the old files stay as archive
static void migrate() {
  static const char* const OLD[] = {"/sdcard/pixel/chat.old.log", "/sdcard/pixel/chat.log"};
  int moved = 0;
  for (int k = 0; k < 2; ++k) {
    FILE* in = fopen(OLD[k], "r");
    if (!in) continue;
    EXT_RAM_BSS_ATTR static char l[1024];
    FILE* out = nullptr;
    while (fgets(l, sizeof(l), in)) {
      if (!strncmp(l, "--- ", 4)) {  // a new conversation
        if (out) fclose(out);
        out = nullptr;
        continue;
      }
      char id[16], p[64];
      if (!out && idFromLine(l, id)) {
        path(id, p, sizeof(p));
        out = fopen(p, "a");
        moved += out != nullptr;
      }
      if (out && (l[0] != '\n' || l[1])) fputs(l, out);
    }
    if (out) fclose(out);
    fclose(in);
    char a[48];
    snprintf(a, sizeof(a), "/sdcard/pixel/chat-archiv-%d.log", k + 1);
    rename(OLD[k], a);
  }
  if (moved) ESP_LOGI(TAG, "%d conversations from the old log", moved);
}

void init() {
  mtx = xSemaphoreCreateMutex();
  if (!sd::ok()) return;
  mkdir(DIR_, 0775);
  migrate();
}

void begin() {
  if (!mtx) return;
  xSemaphoreTake(mtx, portMAX_DELAY);
  cur[0] = 0;
  xSemaphoreGive(mtx);
}
void reopen(const char* id) {
  if (!mtx || !validId(id)) return;
  xSemaphoreTake(mtx, portMAX_DELAY);
  snprintf(cur, sizeof(cur), "%s", id);
  xSemaphoreGive(mtx);
}
void current(char* id, int cap) {
  if (!mtx) {
    id[0] = 0;
    return;
  }
  xSemaphoreTake(mtx, portMAX_DELAY);
  snprintf(id, cap, "%s", cur);
  xSemaphoreGive(mtx);
}
void line(const char* text) {
  if (!mtx) return;
  char p[64];
  xSemaphoreTake(mtx, portMAX_DELAY);
  if (!cur[0] && !idFromLine(text, cur)) {  // the first line names the file (no clock yet: from the uptime)
    time_t t = time(nullptr);
    struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(cur, sizeof(cur), "%Y%m%d-%H%M%S", &tmv);
  }
  path(cur, p, sizeof(p));
  xSemaphoreGive(mtx);
  sd::append(p, text);
}

int ids(char (*out)[16], int max) {
  DIR* d = sd::ok() ? opendir(DIR_) : nullptr;
  if (!d) return 0;
  int n = 0;
  while (struct dirent* e = readdir(d)) {
    size_t l = strlen(e->d_name);
    if (l != 19 || strcmp(e->d_name + 15, ".txt")) continue;
    char id[16];
    memcpy(id, e->d_name, 15);
    id[15] = 0;
    if (!validId(id)) continue;
    if (n < max) {
      memcpy(out[n++], id, 16);
    } else {  // more than fit: keep the newest
      int o = 0;
      for (int i = 1; i < max; ++i)
        if (strcmp(out[i], out[o]) < 0) o = i;
      if (strcmp(id, out[o]) > 0) memcpy(out[o], id, 16);
    }
  }
  closedir(d);
  qsort(out, n, 16, [](const void* a, const void* b) { return strcmp((const char*)a, (const char*)b); });
  return n;
}

int listJson(char* out, int cap) {
  EXT_RAM_BSS_ATTR static char all[MAXC][16];
  EXT_RAM_BSS_ATTR static Ent next[MAXC];
  EXT_RAM_BSS_ATTR static Recent by[MAXC];
  int n = ids(all, MAXC);
  for (int i = 0; i < n; ++i) {  // the one talked in last comes first (a continued one moves up)
    char p[64];
    struct stat st;
    path(all[i], p, sizeof(p));
    by[i].t = stat(p, &st) == 0 ? (long long)st.st_mtime : 0;
    memcpy(by[i].id, all[i], 16);
  }
  qsort(by, n, sizeof(Recent), byRecent);
  for (int i = 0; i < n; ++i) memcpy(all[n - 1 - i], by[i].id, 16);
  char now[16];
  current(now, sizeof(now));
  cJSON* root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "current", now);
  cJSON* items = cJSON_AddArrayToObject(root, "items");
  int m = 0;
  for (int i = n - 1; i >= 0; --i) {
    Ent* e = nullptr;
    for (int k = 0; k < nCache && !e; ++k)
      if (!strcmp(cache[k].id, all[i]) && cache[k].title[0]) e = &cache[k];
    Ent& x = next[m++];
    if (e) {
      x = *e;
    } else {  // not known yet (or still without a title): the start of the file
      char head[1200], p[64];
      path(all[i], p, sizeof(p));
      if (readFile(p, head, sizeof(head)) < 0) head[0] = 0;
      memcpy(x.id, all[i], 16);
      titleOf(head, x.title, sizeof(x.title));
      if (strlen(head) >= 16 && head[4] == '-')
        snprintf(x.at, sizeof(x.at), "%.16s", head);
      else
        snprintf(x.at, sizeof(x.at), "%.4s-%.2s-%.2s %.2s:%.2s", all[i], all[i] + 4, all[i] + 6, all[i] + 9,
                 all[i] + 11);
    }
    cJSON* o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "id", x.id);
    cJSON_AddStringToObject(o, "at", x.at);
    cJSON_AddStringToObject(o, "title", x.title);
    cJSON_AddItemToArray(items, o);
  }
  memcpy(cache, next, sizeof(Ent) * m);
  nCache = m;
  int len = cJSON_PrintPreallocated(root, out, cap, false) ? strlen(out) : -1;
  cJSON_Delete(root);
  return len;
}

long read(const char* id, char* dst, long cap) {
  if (!validId(id)) return -1;
  char p[64];
  path(id, p, sizeof(p));
  return readFile(p, dst, cap);
}

bool remove(const char* id) {
  if (!validId(id)) return false;
  char now[16], p[64];
  current(now, sizeof(now));
  if (!strcmp(now, id)) begin();  // whatever still comes goes to a new one
  path(id, p, sizeof(p));
  return ::remove(p) == 0;
}

void clear() {
  static char all[64][16];
  begin();
  for (;;) {  // in rounds: listing and removing at once is not allowed
    int n = ids(all, 64);
    if (!n) break;
    for (int i = 0; i < n; ++i) remove(all[i]);
  }
  nCache = 0;
  ::remove("/sdcard/pixel/chat-archiv-1.log");  // the old single log, kept after the split
  ::remove("/sdcard/pixel/chat-archiv-2.log");
}

int searchJson(const char* q, char* out, int cap) {
  EXT_RAM_BSS_ATTR static char all[MAXC][16];
  const long BUF = 64 * 1024;
  char* text = (char*)heap_caps_malloc(BUF, MALLOC_CAP_SPIRAM);
  if (!text) return -1;
  char needle[96];
  snprintf(needle, sizeof(needle), "%s", q);
  lower(needle);
  int n = ids(all, MAXC);
  cJSON* arr = cJSON_CreateArray();
  for (int i = n - 1; i >= 0 && needle[0] && cJSON_GetArraySize(arr) < 100; --i) {
    if (read(all[i], text, BUF) <= 0) continue;
    EXT_RAM_BSS_ATTR static char low[64 * 1024];
    memcpy(low, text, strlen(text) + 1);
    lower(low);  // (same length: the snippet comes from the original at the same place)
    const char* hit = strstr(low, needle);
    if (!hit) continue;
    long at = hit - low, b = at + strlen(needle), ls = at;
    while (ls > 0 && text[ls - 1] != '\n') --ls;  // the line: from after its time stamp, up to 50 bytes before
    long from = strlen(text + ls) > 21 && text[ls + 4] == '-' && at >= ls + 21 ? ls + 21 : ls;
    long a = at - 50 > from ? at - 50 : from;
    while (text[b] && b - at < 90 && text[b] != '\n') ++b;
    while (((unsigned char)text[a] & 0xC0) == 0x80) ++a;
    while (b > a && ((unsigned char)text[b] & 0xC0) == 0x80) --b;
    char snip[200];
    snprintf(snip, sizeof(snip), "%.*s", (int)(b - a < 199 ? b - a : 199), text + a);
    cJSON* o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "id", all[i]);
    cJSON_AddStringToObject(o, "snip", snip);
    cJSON_AddItemToArray(arr, o);
  }
  free(text);
  int len = cJSON_PrintPreallocated(arr, out, cap, false) ? strlen(out) : -1;
  cJSON_Delete(arr);
  return len;
}

}  // namespace chats
