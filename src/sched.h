// Time-of-day schedule (display brightness, volume): which entry applies at a minute of the day.
// Pure (tested on the PC by tools/hosttest.cpp).
#pragma once

namespace sched {

// at: start minutes (0..1439, any order). The entry that started last before (or at) this minute; before the
// first one of the day the last one of yesterday still applies. -1 without entries.
inline int current(const int* at, int n, int minute) {
  int best = -1, last = -1;
  for (int i = 0; i < n; ++i) {
    if (at[i] <= minute && (best < 0 || at[i] > at[best])) best = i;
    if (last < 0 || at[i] > at[last]) last = i;
  }
  return best >= 0 ? best : last;
}

}  // namespace sched
