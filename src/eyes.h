// Procedural cartoon eyes: SDF renderer + spring-driven animation rig.
// Platform independent (used by the ESP32 firmware and the desktop simulator).
#pragma once
#include <stdint.h>

namespace eyes {

constexpr int W = 240, H = 240;

struct Rect {
  int x0, y0, x1, y1;
  bool empty() const { return x1 <= x0 || y1 <= y0; }
};

// Everything about the look and the idle behaviour that can be changed at runtime.
struct Look {
  float eyeW = 1.f, eyeH = 1.f;     // eye size (scale)
  float spacing = 1.f;              // distance between the eyes (scale)
  float posY = 0.f;                 // vertical offset in px
  float tilt = 1.f;                 // outward lean (0 = upright)
  float irisW = 1.f, irisH = 1.f;   // iris size (scale)
  float irisEgg = 0.10f;            // iris wider at the bottom
  float highlights = 1.f;           // the two shine dots, 0..1
  float stroke = 6.5f;              // closed-eye line thickness in px
  float breath = 1.f;               // breathing motion
  float blinkMin = 1.6f, blinkMax = 5.5f;   // seconds between blinks
  float gazeActivity = 1.f;         // how often it looks around
  float headFollow = 1.f;           // how much the head follows the eyes
  float idleMin = 3.5f, idleMax = 8.5f;     // pause between idle animations
  float sleepAfter = 240.f;         // fall asleep after s without interaction (0 = never)
  bool blush = true, wobble = true;
  // face: shape and resting expression
  float shape = 0.f;                // 0 round .. 1 rounded square (eyes and irises)
  float pupil = 0.f;                // pupil size, 0 = none
  float lidTop = 0.f;               // resting upper lid (0 open .. 0.6 heavy)
  float lidBottom = 0.f;            // resting lower lid (a soft smile in the eyes)
  float brow = 0.f;                 // resting lid slant: -1 worried .. +1 stern
  float asym = 0.f;                 // one eye a little bigger (0 .. 0.3)
  float squash = 1.f;               // cartoon squash and stretch when moving (0 .. 2)
  // character: how it behaves when nobody talks to it
  float energy = 1.f;               // 0.3 calm .. 2 restless (pauses, glances, wandering)
  float roam = 0.5f;                // 0 stays put .. 1 wanders all over the screen (and closer / farther)
  float cheer = 0.5f, cheek = 0.3f, sleepy = 0.2f, shy = 0.2f, grumpy = 0.f;  // spontaneous moods 0..1
  bool music = true;                // grooves along when music plays nearby
  int colorway = 0;                 // preset index, or -1 = custom colours below
  uint32_t sclera = 0xF6F3EA, iris = 0x265CFF, accent = 0xFF689C, shine = 0xFFFFFF, bg = 0x000000;
};
void setLook(const Look& l);
const Look& look();
int colorwayCount();
const char* colorwayNameAt(int i);
uint32_t colorwayColor(int i, int which);  // 0 sclera, 1 iris, 2 accent
// per animation: on/off, speed (0.3..3) and intensity (0..1)
void setBehavior(int i, bool on, float speed, float intensity);
enum Kind { K_INTERNAL, K_REACTION, K_IDLE, K_TOUCH, K_ERROR, K_HOME };
int behaviorKind(int i);
int behaviorIndex(const char* name);   // -1 if unknown

void init(uint32_t seed);
void update(float dt);                  // advance animation, builds the frame snapshot
float fps();                            // frames per second right now (smoothed)
void touch(bool down, int x, int y);    // call every frame with the current touch state
void buttonClick();                     // next animation (demo)
void buttonLong();                      // next colorway
// microphone: voice detected, loudness 0..1, pitch/brightness 0..1 (call every frame)
void voice(bool active, float level, float pitch);
void wakeWord();                        // wake word heard: start a listening session
void lookAt(float gx, float gy, float secs);  // look that way (-1..1 left/up .. right/down) for a while
void listenDone(bool gotSpeech);        // utterance finished: think (waiting for an answer) or "hm?"
// play a sequence of reactions, names: yes no dont_know happy laugh love excited proud sad cry
// surprised scared angry confused thinking shy wink hello sleep dizzy bored suspicious curious dance
void react(const char* const* names, int n);
bool asleep();
// error animations instead of error messages, each with a symbol in the irises:
// offline (WiFi), quota (empty battery), cant (no-entry sign), huh (question marks),
// locked (padlock), glitch (torn picture + reboot)
void error(const char* name);
// smart home feedback, a light bulb in the irises glowing in the lamp's colour (0xRRGGBB):
// light_on, light_off, light_color (rgb = the new colour)
void home(const char* name, uint32_t rgb);
void battery(int level, bool charging, bool show);  // real charge 0..100 (-1 unknown); show: the animation
void timer(bool ringing);
// music nearby: 0..1, tempo, continuous beat count, loudness 0..1, drive 0..1 (calm .. fast and busy);
// mayWake: it may wake the eyes up (not at night)
void music(float amount, float bpm, float beatPos, float loud, float drive, bool mayWake);
void sequence(const int* behaviors, int n);          // own animation: up to 6 behaviours in a row               // a clock: timer set (hands spin) / alarm ringing (bells)
// weather symbol, Home Assistant condition names: sunny partlycloudy cloudy rainy pouring snowy
// lightning lightning-rainy fog clear-night windy ...
void weather(const char* cond);
void moodLight(bool on, uint32_t rgb);  // irises take on a lamp's colour (fades), off = normal colours
// while listening: the mood of what is being said (happy sad surprised love curious funny worried)
void listenMood(const char* name);
void busy(bool on);                     // waiting for a tool / web search (thinking, open ended)
bool reacting();                        // a reaction or error animation is still in its main part
// while talking: loudness of the own voice 0..1 (call every frame), -1 when done
void speaking(float level);

Rect dirtyRect();                       // region that must be redrawn for this frame
// Render the rectangle [x0,x1)x[y0,y1) into buf (stride x1-x0), RGB565.
// With EYES_SWAP_BYTES defined, pixels are byte swapped for SPI displays.
void render(uint16_t* buf, int x0, int y0, int x1, int y1);

int behaviorCount();
const char* behaviorName(int i);
void trigger(int i);                    // start behavior i now
const char* currentName();
const char* colorwayName();
void bench();                           // ESP only: micro benchmarks over serial

}  // namespace eyes
