// The conversations on the SD card, one file each (/sdcard/pixel/chats/YYYYMMDD-HHMMSS.txt, one line per turn:
// "YYYY-MM-DD HH:MM:SS  Who: text", Who = "You" or the robot's name). The web interface lists, opens, searches,
// exports and deletes them.
#pragma once
#include <ctype.h>
#include <stdint.h>
#include <string.h>

namespace chats {

void init();                          // after the SD card is mounted: folder, the old single log split up once
void begin();                         // a new conversation: its file starts with the next line
void line(const char* text);          // one log line of the current conversation
void reopen(const char* id);          // continue that one: the next lines go there
void current(char* id, int cap);      // the conversation the next lines go to ("" = a new one)
int listJson(char* out, int cap);     // {"current":id,"items":[{"id","at","title"}]} last talked in first
long read(const char* id, char* dst, long cap);  // the whole file (0-terminated), -1 none
bool remove(const char* id);
void clear();                         // all of them
int searchJson(const char* q, char* out, int cap);  // [{"id","snip"}] newest first
int ids(char (*out)[16], int max);    // all ids, oldest first

// ---- pure helpers (tested on the PC by tools/hosttest.cpp)
// a conversation id: 8 digits, '-', 6 digits
inline bool validId(const char* id) {
  if (!id || strlen(id) != 15 || id[8] != '-') return false;
  for (int i = 0; i < 15; ++i)
    if (i != 8 && (id[i] < '0' || id[i] > '9')) return false;
  return true;
}
// "2026-10-19 20:15:30  Du: ..." -> "20261019-201530"; false if the line has no time stamp
inline bool idFromLine(const char* l, char* id) {
  static const int D[] = {0, 1, 2, 3, 5, 6, 8, 9, -1, 11, 12, 14, 15, 17, 18};
  if (strlen(l) < 21 || l[4] != '-' || l[7] != '-' || l[10] != ' ' || l[13] != ':' || l[16] != ':') return false;
  for (int i = 0; i < 15; ++i) {
    if (D[i] < 0) {
      id[i] = '-';
      continue;
    }
    if (l[D[i]] < '0' || l[D[i]] > '9') return false;
    id[i] = l[D[i]];
  }
  id[15] = 0;
  return true;
}
// the title of a conversation: what the person said first (else the first line), at most cap-1 bytes, whole
// UTF-8 characters
inline void titleOf(const char* text, char* out, int cap) {
  const char* t = strstr(text, "  You: ");
  const char* s = t ? t + 7 : nullptr;
  if (!s && (t = strstr(text, "  Du: "))) s = t + 6;  // (German logs up to 1.1)
  if (!s) {
    const char* c = strstr(text, ": ");
    s = c && c - text < 40 ? c + 2 : "";
  }
  int n = 0;
  while (s[n] && s[n] != '\n' && n < cap - 1) ++n;
  while (n > 0 && n < (int)strlen(s) && ((unsigned char)s[n] & 0xC0) == 0x80) --n;  // not inside a character
  memcpy(out, s, n);
  out[n] = 0;
}
// the order of the list: the one written to last first, the same time (FAT: 2 s) the newer id first
struct Recent {
  long long t;  // the file's last change
  char id[16];
};
inline int byRecent(const void* a, const void* b) {
  const Recent &x = *(const Recent*)a, &y = *(const Recent*)b;
  return x.t != y.t ? (x.t > y.t ? -1 : 1) : -strcmp(x.id, y.id);
}
// lower case for searching: ASCII and the German umlauts (in place)
inline void lower(char* s) {
  for (unsigned char* p = (unsigned char*)s; *p; ++p) {
    if (*p < 128) *p = (unsigned char)tolower(*p);
    else if (*p == 0xC3 && (p[1] == 0x84 || p[1] == 0x96 || p[1] == 0x9C)) p[1] += 0x20, ++p;  // Ä Ö Ü
  }
}

}  // namespace chats
