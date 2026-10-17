// Second safety net for sensitive self management: the model alone may not restart, switch off,
// clear the history or stop listening - what the person said must ask for it too.
// Pure (tested on the PC by tools/hosttest.cpp).
#pragma once
#include <stdio.h>
#include <string.h>

namespace guard {

// said: the person's words in lower case. Entries "a+b": both parts somewhere ("starte dich bitte neu").
inline bool askedFor(const char* action, const char* said) {
  static const struct {
    const char* action;
    const char* words[10];
  } W[] = {
      {"restart", {"start+neu", "neustart", "restart", "reboot"}},
      {"shutdown", {"dich+aus", "fahr+runter", "herunterfahr", "shutdown"}},  // not "mach das licht aus"
      // "vergiss es" means "never mind": deleting needs to name what goes
      {"clear_history", {"verlauf+l\xc3\xb6sch", "verlauf+loesch", "verlauf+vergiss", "protokoll+l\xc3\xb6sch",
                         "protokoll+loesch", "gespr\xc3\xa4" "ch+l\xc3\xb6sch", "gespraech+loesch", "vergiss+alles"}},
      {"forget_all", {"vergiss+alles", "l\xc3\xb6sch+alles", "loesch+alles"}},
      // installing restarts it: only on "installier ...", "aktualisier dich", "mach / spiel / lad das update"
      {"update_install", {"installier", "aktualisier", "update+mach", "update+spiel", "update+lad", "update+starte"}},
      // "ruhe" ends a conversation, it must not switch off listening
      {"privacy", {"h\xc3\xb6r", "hoer", "zuh\xc3\xb6r", "privat", "mikro", "lausch"}},
  };
  for (auto& w : W) {
    if (strcmp(w.action, action)) continue;
    for (const char* k : w.words) {
      if (!k) break;
      char part[24];
      bool all = true;
      for (const char* q = k; *q && all;) {
        const char* e = strchr(q, '+');
        int n = e ? (int)(e - q) : (int)strlen(q);
        snprintf(part, sizeof(part), "%.*s", n, q);
        all = strstr(said, part) != nullptr;
        q += n + (e ? 1 : 0);
      }
      if (all) return true;
    }
    return false;
  }
  return true;  // not a sensitive action
}

}  // namespace guard
