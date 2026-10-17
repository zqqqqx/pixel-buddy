// Small household helpers for the voice assistant: timers / alarms, a memory of facts about the
// person, and what the OpenAI usage costs. Kept on the SD card (in RAM without one).
#pragma once
#include <time.h>

namespace assist {

void init();
void wipe();  // factory reset: timers, memory and usage are gone

// timers and alarms (wall clock). Returns false when full / invalid.
bool addTimer(int seconds, const char* label);
bool addAlarm(int hour, int minute, const char* label);
int cancel(const char* label);            // "" or "alle" = all; returns how many were removed
void listTimers(char* out, int cap);      // for the model, German
bool popDue(char* label, int cap);        // a timer that just went off (call often)
int timerCount();

// memory ("merk dir ...")
bool remember(const char* fact);         // false: the memory is full
int forget(const char* what);             // removes lines containing it ("" = everything), returns count
const char* memory();                     // all facts, one per line (for the prompt)

// usage in USD (estimates from the token / second counts the API reports)
void addCost(double usd);
double today();
double month();
bool overBudget(double perDay);           // perDay 0 = no limit

// the next hour:minute in local time (today if still ahead, else tomorrow; DST changes included)
inline time_t nextAlarm(time_t now, int hour, int minute) {
  struct tm t;
  localtime_r(&now, &t);
  t.tm_hour = hour;
  t.tm_min = minute;
  t.tm_sec = 0;
  t.tm_isdst = -1;
  time_t due = mktime(&t);
  if (due <= now) {
    t.tm_mday += 1;
    t.tm_hour = hour;
    t.tm_min = minute;
    t.tm_isdst = -1;
    due = mktime(&t);
  }
  return due;
}

}  // namespace assist
