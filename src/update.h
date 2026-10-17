// Updates from GitHub: the latest release of PIXEL_REPO, its asset "pixel-update.bin". Automatic at a fixed time
// (settings), or on request (web interface, voice).
#pragma once

namespace update {

enum State { IDLE, CHECKING, UP_TO_DATE, AVAILABLE, DOWNLOADING, DONE, FAILED };

void check(bool install);   // in the background; install: when a newer version is found, install it right away
void install();             // the version found by the last check
State state();
int progress();             // 0..100 while downloading
const char* latest();       // "1.2.0" once checked
void json(char* out, int cap);  // for the web interface
// pure (tested on the PC): -1 / 0 / 1 for a < / = / > b, e.g. "v1.10.2" > "1.9.9"; anything after the
// numbers ("-beta") is ignored
inline int compare(const char* a, const char* b) {
  if (*a == 'v' || *a == 'V') ++a;
  if (*b == 'v' || *b == 'V') ++b;
  for (int i = 0; i < 3; ++i) {
    long x = 0, y = 0;
    while (*a >= '0' && *a <= '9') x = x * 10 + (*a++ - '0');
    while (*b >= '0' && *b <= '9') y = y * 10 + (*b++ - '0');
    if (x != y) return x < y ? -1 : 1;
    if (*a == '.') ++a;
    if (*b == '.') ++b;
  }
  return 0;
}

}  // namespace update
