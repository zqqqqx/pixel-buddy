// Fluent voice conversation over OpenAI GPT-Live (WebSocket, audio streamed both ways).
// The voice model delegates tools (web search, changing the look, smart home) to a cheap,
// fast text model; transcript triggers pick the eye animations while it talks and listens.
#pragma once
#include <stdint.h>

namespace live {

enum Phase { IDLE, CONNECTING, WAITING, USER_TALKING, RESPONDING };

// what the eyes should do, produced by the session, consumed by the main loop
enum EvKind { EV_REACT, EV_MOOD, EV_ERROR, EV_BUSY, EV_PLAY, EV_HOME, EV_WEATHER, EV_SYS };
struct Event {
  int kind;
  char name[32];  // reaction / mood / error name, EV_BUSY: "1" or "0", EV_PLAY: asked for by voice
  uint32_t rgb;   // EV_HOME: the lamp's colour 0xRRGGBB (name: light_on, light_off, light_color)
                  // EV_WEATHER: name = condition; EV_SYS: name = sleep, privacy, shutdown
};

void start();                       // session task; keeps a warm connection while idle
void begin(bool wakeWord = true);   // start buffering the microphone and open a session
bool active();                      // a session is open (or being opened)
Phase phase();
bool popEvent(Event& e);
void forget();                      // the chat log was cleared: the next session starts without history
bool resumeChat(const char* id);    // continue that conversation (web interface); false while one runs
void newChat();                     // the next conversation starts fresh, in a new file
void say(const char* text);         // typed in the web interface: sent as the person's turn (session follows)
void interrupt();                   // touched while it talks: stop the answer
void requestWeather();
void rest();                        // close the warm connections for a while (an update needs the memory)              // show the current weather (Home Assistant) on the eyes

}  // namespace live
