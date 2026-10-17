// Desktop simulator: renders the eye engine to raw RGB24 frames on stdout.
//   sim auto <seconds>            free running (random behaviours)
//   sim all                       every behaviour once, in order
//   sim one <index> [seconds]     a single behaviour
//   sim touch                     scripted taps / petting
// Pipe into ffmpeg: ./sim all | ffmpeg -f rawvideo -pix_fmt rgb24 -s 240x240 -r 50 -i - out.mp4
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../src/eyes.h"

static uint16_t fb[eyes::W * eyes::H];
static uint16_t band[eyes::W * 40];
static double renderSec = 0;
static long renderPx = 0;

static void frame(float dt) {
  eyes::update(dt);
  eyes::Rect r = eyes::dirtyRect();
  auto t0 = std::chrono::steady_clock::now();
  for (int y = r.y0; y < r.y1; y += 40) {
    int y1 = y + 40 < r.y1 ? y + 40 : r.y1;
    int w = r.x1 - r.x0;
    eyes::render(band, r.x0, y, r.x1, y1);
    for (int yy = y; yy < y1; ++yy) memcpy(&fb[yy * eyes::W + r.x0], &band[(yy - y) * w], w * 2);
    renderPx += (long)w * (y1 - y);
  }
  renderSec += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  static uint8_t rgb[eyes::W * eyes::H * 3];
  for (int i = 0; i < eyes::W * eyes::H; ++i) {
    uint16_t c = fb[i];
    rgb[i * 3 + 0] = (uint8_t)(((c >> 11) & 31) * 255 / 31);
    rgb[i * 3 + 1] = (uint8_t)(((c >> 5) & 63) * 255 / 63);
    rgb[i * 3 + 2] = (uint8_t)((c & 31) * 255 / 31);
  }
  fwrite(rgb, sizeof(rgb), 1, stdout);
}

int main(int argc, char** argv) {
  std::string mode = argc > 1 ? argv[1] : "auto";
  const float fps = 50.f, dt = 1.f / fps;
  eyes::init(12345);
  // SIM_LOOK="shape=0.8 pupil=0.6 roam=1 ..." tries out a face / character
  if (const char* lk = getenv("SIM_LOOK")) {
    eyes::Look l;
    struct { const char* k; float* v; } F[] = {
        {"shape", &l.shape}, {"pupil", &l.pupil}, {"lidTop", &l.lidTop}, {"lidBottom", &l.lidBottom},
        {"brow", &l.brow}, {"asym", &l.asym}, {"squash", &l.squash}, {"energy", &l.energy}, {"roam", &l.roam},
        {"cheer", &l.cheer}, {"cheek", &l.cheek}, {"sleepy", &l.sleepy}, {"shy", &l.shy}, {"grumpy", &l.grumpy},
        {"eyeW", &l.eyeW}, {"eyeH", &l.eyeH}, {"irisW", &l.irisW}, {"irisH", &l.irisH}, {"spacing", &l.spacing}};
    for (auto& f : F) {
      const char* p = strstr(lk, f.k);
      if (p && p[strlen(f.k)] == '=') *f.v = atof(p + strlen(f.k) + 1);
    }
    if (const char* c = strstr(lk, "colorway=")) l.colorway = atoi(c + 9);
    eyes::setLook(l);
  }
  if (mode == "auto") {
    float secs = argc > 2 ? atof(argv[2]) : 30.f;
    int last = -2;
    for (int f = 0; f < secs * fps; ++f) {
      frame(dt);
      std::string n = eyes::currentName();
      static std::string ln;
      if (n != ln) fprintf(stderr, "%6.2fs %s\n", f / fps, n.c_str()), ln = n;
      (void)last;
    }
  } else if (mode == "all") {
    int fr = 0;
    for (int f = 0; f < 2.0f * fps; ++f) frame(dt), ++fr;  // wake-up
    for (int b = 0; b < eyes::behaviorCount(); ++b) {
      if (std::string(eyes::behaviorName(b)) == "asleep") continue;  // never ends
      eyes::trigger(b);
      fprintf(stderr, "frame %d: %d %s\n", fr, b, eyes::behaviorName(b));
      int n = 0;
      while (std::string(eyes::currentName()) != "idle" && n < 12 * fps) frame(dt), ++n, ++fr;
      for (int f = 0; f < 0.6f * fps; ++f) frame(dt), ++fr;
    }
  } else if (mode == "one") {
    int b = argc > 2 ? atoi(argv[2]) : 0;
    if (const char* bl = getenv("SIM_BATTERY")) eyes::battery(atoi(bl), strchr(bl, 'c') != nullptr, false);
    float secs = argc > 3 ? atof(argv[3]) : 0.f;
    for (int f = 0; f < 1.0f * fps; ++f) frame(dt);
    if (getenv("SIM_AWAKE")) {  // skip the wake-up: start from open eyes (not drawn)
      for (int f = 0; f < 4.0f * fps; ++f) eyes::update(dt);
    }
    eyes::trigger(b);
    int n = 0;
    while ((secs > 0 ? n < secs * fps : std::string(eyes::currentName()) != "idle") && n < 15 * fps) frame(dt), ++n;
    for (int f = 0; f < 0.5f * fps; ++f) frame(dt);
  } else if (mode == "music") {  // music <bpm> <drive 0..1> <loud 0..1> [seconds]: 2 s idle, then music
    float bpm = argc > 2 ? atof(argv[2]) : 120.f, drive = argc > 3 ? atof(argv[3]) : 0.5f;
    float loud = argc > 4 ? atof(argv[4]) : 0.5f, secs = argc > 5 ? atof(argv[5]) : 14.f;
    for (int f = 0; f < secs * fps; ++f) {
      float t = f / fps;
      eyes::music(t > 2.f ? 1.f : 0.f, bpm, t * bpm / 60.f, loud, drive, true);
      frame(dt);
    }
  } else if (mode == "voice") {
    // 2s idle, ~4s of "speech" (syllables at ~4/s with short pauses), then silence
    for (int f = 0; f < 6.0f * fps; ++f) frame(dt);
    for (int f = 0; f < 10.f * fps; ++f) {
      float t = f / fps;
      bool talking = t < 4.2f && !(t > 1.6f && t < 1.9f);
      float syl = fabsf(sinf(3.14159f * 4.f * t));
      float level = talking ? 0.25f + 0.7f * syl * syl : 0.f;
      float pitch = 0.5f + 0.3f * sinf(t * 2.3f);
      eyes::voice(talking, level, pitch);
      frame(dt);
    }
  } else if (mode == "chat") {
    // wake word -> speech -> waiting for Gemini -> reaction sequence
    for (int f = 0; f < 2.0f * fps; ++f) frame(dt);
    eyes::wakeWord();
    for (int f = 0; f < 3.5f * fps; ++f) {
      float t = f / fps;
      bool talking = t > 0.4f && t < 2.6f;
      float syl = fabsf(sinf(3.14159f * 4.f * t));
      eyes::voice(talking, talking ? 0.25f + 0.7f * syl * syl : 0.f, 0.5f), frame(dt);
    }
    eyes::voice(false, 0, 0.5f);
    eyes::listenDone(true);
    for (int f = 0; f < 3.0f * fps; ++f) frame(dt);
    const char* a[] = {argc > 2 ? argv[2] : "yes", argc > 3 ? argv[3] : "happy"};
    eyes::react(a, 2);
    for (int f = 0; f < 7.f * fps; ++f) frame(dt);
  } else if (mode == "err") {
    // sim err <name> <variation-seed> [rrggbb]: one error animation (or light_on / light_off / light_color)
    eyes::init(argc > 3 ? atoi(argv[3]) : 1);
    for (int f = 0; f < 1.0f * fps; ++f) frame(dt);
    const char* name = argc > 2 ? argv[2] : "offline";
    if (!strcmp(name, "timer") || !strcmp(name, "alarm"))
      eyes::timer(name[0] == 'a');
    else if (!strncmp(name, "weather:", 8))
      eyes::weather(name + 8);
    else if (!strncmp(name, "light", 5))
      eyes::home(name, argc > 4 ? strtoul(argv[4], nullptr, 16) : 0xFF3060);
    else
      eyes::error(name);
    int n = 0;
    while (std::string(eyes::currentName()) != "idle" && n < 12 * fps) frame(dt), ++n;
    for (int f = 0; f < 0.4f * fps; ++f) frame(dt);
  } else if (mode == "mood") {
    // sim mood <name>: listening with a mood
    for (int f = 0; f < 1.0f * fps; ++f) frame(dt);
    eyes::wakeWord();
    for (int f = 0; f < 4.f * fps; ++f) {
      float t = f / fps;
      float syl = fabsf(sinf(3.14159f * 4.f * t));
      if (t > 0.8f) eyes::listenMood(argc > 2 ? argv[2] : "happy");
      eyes::voice(true, 0.25f + 0.6f * syl * syl, 0.5f), frame(dt);
    }
  } else if (mode == "touch") {
    for (int f = 0; f < 1.5f * fps; ++f) frame(dt);
    // single taps at different places
    int taps[][2] = {{60, 60}, {190, 180}, {120, 120}};
    for (auto& tp : taps) {
      for (int f = 0; f < 0.12f * fps; ++f) eyes::touch(true, tp[0], tp[1]), frame(dt);
      eyes::touch(false, 0, 0);
      for (int f = 0; f < 1.6f * fps; ++f) frame(dt);
    }
    // drag across the screen
    for (int f = 0; f < 1.5f * fps; ++f) {
      float u = f / (1.5f * fps);
      eyes::touch(true, (int)(20 + 200 * u), (int)(200 - 160 * u)), frame(dt);
    }
    eyes::touch(false, 0, 0);
    for (int f = 0; f < 1.f * fps; ++f) frame(dt);
    // petting
    for (int f = 0; f < 3.5f * fps; ++f) eyes::touch(true, 120, 60), frame(dt);
    eyes::touch(false, 0, 0);
    for (int f = 0; f < 5.f * fps; ++f) frame(dt);
  }
  fprintf(stderr, "avg render: %.1f us/frame, %.1f ns/px\n", renderSec * 1e6 / 1, renderSec * 1e9 / renderPx);
  return 0;
}
