// ES8311 microphone + simple voice activity detection.
#pragma once
#include <stdint.h>

namespace mic {

struct State {
  bool voice;    // speech detected (with hangover)
  float level;   // loudness above the noise floor, 0..1
  float pitch;   // spectral brightness (zero-crossing based), 0..1
  float dbfs;    // frame energy
  float noise;   // adaptive noise floor
};

bool start();    // codec + I2S + analysis task; false if the codec does not answer
State get();
void setDebug(bool on);
int wakeWords();  // increments on every detected wake word
void setWake(bool on);
// active wake words: "Pixel" (own network) and/or "Hallo ESP" (WakeNet), Pixel sensitivity 0..1
void setWakeWords(bool pixel, bool halloEsp, float sensitivity);
int record(int16_t* dst, int samples);   // blocking: the next samples of the microphone (wake word dataset)
void fakeWake();
// music nearby: confidence 0..1, tempo, continuous beat position, how loud 0..1, how driving 0..1
void music(float& conf, float& bpm, float& pos, float& loud, float& drive);                          // as if the wake word was heard (serial 'w')
int wakeClip(int16_t* dst, int cap);
bool wakeClipPixel();                     // that activation was "Pixel" (not "Hallo ESP")
const int KWS_MAX_FILES = 800;            // recordings kept for the wake word (automatic ones stop there)     // the last activation: 2 s up to the wake word + 4 s after it

// speaker
bool audible();       // speech is being played (or about to be)
float speakLevel();   // loudness of what is being played, 0..1

// streaming playback (16 kHz) and microphone uplink for live conversations
void streamPush(const int16_t* pcm16k, int samples);
void streamFlush();                      // drop queued audio
int streamQueued();                      // samples still queued (not played yet)
// a tone (does not count as speech: no listening pause, no talking animation); level 0..1
void beep(int hz, int ms, float level);
void setStreamContinuous(bool on);       // true: real-time paced stream (drift is compensated in silences)
void setVolume(int percent);             // speaker volume 0..100
void setUplink(bool on);                 // start/stop copying mic audio into the uplink buffer
int uplinkRead(int16_t* dst, int maxSamples, int timeoutMs);
bool uplinkMuted();                      // true while the speaker is (just) active

}  // namespace mic
