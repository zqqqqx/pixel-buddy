// Second safety net for sensitive self management: the model alone may not restart, switch off,
// clear the history or stop listening - what the person said must ask for it too.
// Pure (tested on the PC by tools/hosttest.cpp).
#pragma once
#include <stdio.h>
#include <string.h>

namespace guard {

// said: the person's words in lower case. Entries "a+b": both parts somewhere ("starte dich bitte neu");
// "=ab": the whole utterance, letters only ("Ausschalten." alone, but not "Licht ausschalten").
inline bool askedFor(const char* action, const char* said) {
  static const struct {
    const char* action;
    const char* words[24];
  } W[] = {
      // German and English (the language is a setting; what the person says decides)
      {"restart", {"start+neu", "neustart", "restart", "reboot", "start+again", "start+over"}},
      {"shutdown", {"dich+aus", "fahr+runter", "herunterfahr", "shutdown", "shut+down", "switch+yourself+off",
                    "turn+yourself+off", "power+off", "=ausschalten", "=pixelausschalten", "=ausschaltenbitte",
                    "=bitteausschalten", "=schaltaus", "=schalteaus", "=ausmachen", "=gehaus", "=switchoff",
                    "=turnoff", "=poweroff", "=switchoffplease", "=pleaseswitchoff"}},
      // "vergiss es" / "forget it" mean "never mind": deleting needs to name what goes
      {"clear_history", {"verlauf+l\xc3\xb6sch", "verlauf+loesch", "verlauf+vergiss", "protokoll+l\xc3\xb6sch",
                         "protokoll+loesch", "gespr\xc3\xa4" "ch+l\xc3\xb6sch", "gespraech+loesch", "vergiss+alles",
                         "delete+history", "clear+history", "erase+history", "delete+chat", "delete+conversation",
                         "clear+chat", "clear+conversation", "forget+everything"}},
      {"forget_all", {"vergiss+alles", "l\xc3\xb6sch+alles", "loesch+alles", "forget+everything", "delete+everything"}},
      // installing restarts it: only on "installier ...", "aktualisier dich", "install", "update yourself" ...
      {"update_install", {"installier", "aktualisier", "update+mach", "update+spiel", "update+lad", "update+starte",
                          "install", "update+yourself", "do+the+update", "run+the+update"}},
      // "ruhe" / "quiet" end a conversation, they must not switch off listening
      {"privacy", {"h\xc3\xb6r", "hoer", "zuh\xc3\xb6r", "privat", "mikro", "lausch", "listen", "privacy", "microphone",
                   "mic"}},
  };
  char letters[64];
  int n = 0;
  for (const char* q = said; *q && n < (int)sizeof(letters) - 1; ++q)
    if (*q >= 'a' && *q <= 'z') letters[n++] = *q;
  letters[n] = 0;
  for (auto& w : W) {
    if (strcmp(w.action, action)) continue;
    for (const char* k : w.words) {
      if (!k) break;
      if (k[0] == '=') {
        if (!strcmp(letters, k + 1)) return true;
        continue;
      }
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
