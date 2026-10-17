// PC test of the firmware's pure logic (no ESP needed):
//   g++ -std=c++17 -Isrc tools/hosttest.cpp -o /tmp/hosttest && /tmp/hosttest
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "assist.h"
#include "guard.h"
#include "net.h"
#include "sched.h"
#include "beat.h"
#include <math.h>

int main() {
  // sensitive actions need the person's own words (lower case, as the firmware passes them)
  using guard::askedFor;
  assert(askedFor("restart", "starte dich bitte neu"));
  assert(askedFor("restart", "mach mal einen neustart"));
  assert(!askedFor("restart", "wie spät ist es?"));
  assert(askedFor("shutdown", "schalte dich bitte aus"));
  assert(!askedFor("shutdown", "mach das licht aus"));
  assert(askedFor("clear_history", "lösch den verlauf"));
  assert(!askedFor("clear_history", "ach, vergiss es"));
  assert(askedFor("clear_history", "lösch bitte das gespräch"));
  assert(askedFor("forget_all", "vergiss alles über mich"));
  assert(askedFor("privacy", "hör bitte nicht mehr zu"));
  assert(!askedFor("privacy", "ruhe jetzt"));
  assert(askedFor("status", "egal"));  // not sensitive

  // brightness / volume schedule: the latest entry so far, before the first one of the day yesterday's last
  const int at[] = {19 * 60, 7 * 60, 22 * 60};  // in any order
  assert(sched::current(at, 3, 12 * 60) == 1);
  assert(sched::current(at, 3, 19 * 60) == 0);  // from the minute on
  assert(sched::current(at, 3, 23 * 60 + 59) == 2);
  assert(sched::current(at, 3, 3 * 60) == 2);  // after midnight: still 22:00
  assert(sched::current(at, 0, 600) == -1);
  const int one[] = {22 * 60};
  assert(sched::current(one, 1, 0) == 0 && sched::current(one, 1, 23 * 60) == 0);

  // music: a steady beat is found with its tempo, onsets at random times (speech) are not music
  {
    auto run = [](float bpm, float jitter) {
      static Beat b;
      b = Beat();
      unsigned r = 1;
      auto rnd = [&]() { return (r = r * 1664525u + 1013904223u) / 4294967296.f; };
      float next = 0;
      for (int f = 0; f < 50 * 15; ++f) {  // 15 s of 20 ms frames: spectral flux = noise + a hit on every beat
        float o = 4.f * rnd();
        if (f >= next) {
          o += 25.f;
          next += 3000.f / bpm * (1.f + jitter * (2.f * rnd() - 1.f));
        }
        b.feed(o);
      }
      return b;
    };
    Beat m = run(120, 0);
    assert(m.conf > 0.5f && fabsf(m.bpm() - 120) < 6);
    Beat fast = run(150, 0);
    assert(fast.conf > 0.5f && fabsf(fast.bpm() - 150) < 8);
    assert(run(120, 0.6f).conf < 0.5f);
    Beat slow = run(55, 0);  // every other beat only (the snare through a small speaker): nods at half the tempo
    assert(slow.conf > 0.5f && fabsf(slow.bpm() - 55) < 4);
  }

  // captive portal DNS: the answer must go right after the question, also with an EDNS0 record behind it
  const unsigned char q[] = {0x12, 0x34, 1, 0, 0, 1, 0, 0, 0, 0, 0, 1,                // header, 1 additional
                             7, 'e', 'x', 'a', 'm', 'p', 'l', 'e', 3, 'c', 'o', 'm', 0,  // name
                             0, 1, 0, 1,                                                 // A, IN
                             0, 0, 41, 16, 0, 0, 0, 0, 0, 0, 0};                         // EDNS0 OPT
  assert(net::dnsQuestionEnd(q, sizeof(q)) == 12 + 13 + 4);
  assert(net::dnsQuestionEnd(q, 20) == -1);  // cut off in the middle of the name

  // alarms: today if still ahead, else tomorrow - also across the DST switch (29 March 2026, 02:00 -> 03:00)
  setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
  tzset();
  struct tm t = {};
  t.tm_year = 2026 - 1900, t.tm_mon = 2, t.tm_mday = 28, t.tm_hour = 22, t.tm_isdst = -1;  // Sat 22:00
  time_t now = mktime(&t);
  time_t a = assist::nextAlarm(now, 7, 0);
  struct tm r;
  localtime_r(&a, &r);
  assert(r.tm_mday == 29 && r.tm_hour == 7 && r.tm_min == 0);  // Sunday 07:00 (summer time)
  assert(a - now == 8 * 3600);                                  // only 8 h: one hour lost
  a = assist::nextAlarm(now, 23, 30);
  localtime_r(&a, &r);
  assert(r.tm_mday == 28 && r.tm_hour == 23 && r.tm_min == 30);  // later today
  puts("hosttest ok");
}
