#include "i18n.h"

#include "settings.h"

namespace i18n {

// one row per text, one column per language (the order of LANGS); "" = the English one
static const char* const TEXT[T_COUNT][N_LANG] = {
    {"Restart", "Neustart"},
    {"Hotspot", "Hotspot"},
    {"Home WiFi", "Heimnetz"},
    {"Off", "Aus"},
    {"Reset", "Reset"},
    {"Yes", "Ja"},
    {"No", "Nein"},
    {"OK", "OK"},
    {"WiFi", "WLAN"},
    {"Restart?", "Neu starten?"},
    {"Back in a moment.", "Gleich wieder da."},
    {"No hotspot", "Kein Hotspot"},
    {"Set it up at\npixel.local", "Einrichten unter\npixel.local"},
    {"Use the hotspot?", "Zum Hotspot?"},
    {"Connecting to\n%s", "Verbinde mich mit\n%s"},
    {"Back home?", "Zum Heimnetz?"},
    {"Back to your\nhome WiFi.", "Zurück in dein\nWLAN zu Hause."},
    {"Switch off?", "Ausschalten?"},
    {"To turn on:\npress BOOT.", "Wieder an:\nTaste BOOT drücken."},
    {"Erase all?", "Alles löschen?"},
    {"Resets everything:\nWiFi, key, chats", "Setzt alles zurück:\nWLAN, Key, Verlauf"},
    {"Erase", "Löschen"},
    {"See you!", "Bis gleich!"},
    {"Good night", "Gute Nacht"},
    {"Fresh start", "Alles neu"},
    {"restarting …", "starte neu …"},
    {"erasing …", "lösche …"},
    {"Update %s", "Update %s"},
    {"Please keep me on", "Bitte nicht ausschalten"},
    {"Done – restarting", "Fertig – starte neu"},
    {"Set up WiFi", "WLAN einrichten"},
    {"Join %s", "Verbinde dich mit %s"},
    {"Password %s", "Passwort %s"},
    {"then open 192.168.4.1", "dann 192.168.4.1 öffnen"},
    {"Connected!", "Verbunden!"},
    {"pixel.local", "pixel.local"},
};

const char* t(Text id) {
  int l = index(settings::lang());
  const char* s = l > 0 ? TEXT[id][l] : nullptr;
  return s && s[0] ? s : TEXT[id][0];
}

}  // namespace i18n
