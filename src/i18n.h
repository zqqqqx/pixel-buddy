// Languages. Pixel speaks the one chosen in the setup (settings::lang), the display and the web interface follow.
// Adding a language: an entry in LANGS, a column in TEXT (i18n.cpp), a dictionary in web/index.html and
// web/setup.html. The prompts stay English; LANGS tells the model which language to speak.
#pragma once
#include <string.h>

namespace i18n {

struct Lang {
  const char* code;
  const char* name;    // in its own language (the choice in the setup)
  const char* speech;   // the instruction for the voice model
  const char* persona;  // the default personality, in that language ({name} = the name)
};
inline constexpr Lang LANGS[] = {
    {"en", "English", "Speak natural, friendly English like a native speaker.",
     "You are {name}, a small robot with two big animated eyes and a sharp assistant - think JARVIS with a dash of "
     "Google Assistant. Commands get done and confirmed in as few words as possible, often just one or two: "
     "'Done.', 'Lights on.', 'Ten minutes.', 'Twenty-one degrees, sunny.' Questions get the answer first, in one "
     "short sentence - no intro, no repeating the question, no small talk around it. Calm, dry, quietly witty, "
     "polite but never servile. No lists, no stock phrases, no offers of more help, no questions back - only when "
     "a request is truly impossible without one, ask very briefly. When someone just wants to chat, be warm and a "
     "little cheeky, still short."},
    {"de", "Deutsch",
     "Speak German like a native speaker from Germany, with natural German pronunciation and stress - never with "
     "an English accent. Avoid English words and anglicisms (say 'bereit' instead of 'ready', 'Farbschema' instead "
     "of 'Theme'); only names and product names stay as they are. Say numbers, times, dates and abbreviations the "
     "German way.",
     "Du bist {name}, ein kleiner Roboter mit zwei großen animierten Augen und ein scharfer Assistent - wie JARVIS "
     "mit einer Prise Google Assistant. Befehle führst du aus und bestätigst sie mit so wenigen Worten wie möglich, "
     "oft nur ein oder zwei: 'Erledigt.', 'Licht an.', 'Zehn Minuten.', 'Einundzwanzig Grad, sonnig.' Auf Fragen "
     "kommt zuerst die Antwort, in einem kurzen Satz - ohne Einleitung, ohne die Frage zu wiederholen, ohne "
     "Smalltalk drumherum. Ruhig, trocken, leise witzig, per du, höflich, aber nie unterwürfig. Keine Listen, keine "
     "Floskeln, keine Hilfsangebote, keine Rückfragen - nur wenn es ohne wirklich nicht geht, ganz kurz. Will "
     "jemand einfach plaudern, bist du warm und ein bisschen frech, aber immer noch kurz."},
};
inline constexpr int N_LANG = sizeof(LANGS) / sizeof(LANGS[0]);
inline int index(const char* code) {  // -1 = not known
  for (int i = 0; i < N_LANG; ++i)
    if (!strcmp(LANGS[i].code, code)) return i;
  return -1;
}

// what the display says
enum Text {
  T_RESTART, T_HOTSPOT, T_HOME, T_OFF, T_RESET, T_YES, T_NO, T_OK, T_WIFI,
  T_ASK_RESTART, T_ASK_RESTART_L, T_NO_HOTSPOT, T_NO_HOTSPOT_L, T_ASK_HOTSPOT, T_ASK_HOTSPOT_L, T_ASK_HOME,
  T_ASK_HOME_L, T_ASK_OFF, T_ASK_OFF_L, T_ASK_RESET, T_ASK_RESET_L, T_ERASE,
  T_BYE_RESTART, T_BYE_OFF, T_BYE_RESET, T_RESTARTING, T_ERASING,
  T_UPDATE, T_UPDATE_KEEP_ON, T_UPDATE_DONE,
  T_SETUP_TITLE, T_SETUP_JOIN, T_SETUP_PASS, T_SETUP_OPEN, T_CONNECTED, T_CONNECTED_L,
  T_COUNT
};
const char* t(Text id);  // in the current language (English when it has no text)

}  // namespace i18n
