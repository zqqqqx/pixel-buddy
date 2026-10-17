// Music nearby: beat tracking from the microphone. Onset strength every 20 ms (spectral flux: how much the
// log-mel bands of the wake word features rise) -> tempo by autocorrelation (35-176 BPM: through small speakers
// the snare on every other beat is often the clearest) -> a beat clock that locks onto the onsets. Speech has
// onsets too, but not at a steady tempo, so its autocorrelation stays low.
// Pure (tested on the PC by tools/hosttest.cpp).
#pragma once
#include <math.h>

struct Beat {
  static const int N = 256;          // 5 s of onsets at 50 frames/s
  static const int LMIN = 17, LMAX = 86;  // beat period in frames: 176 .. 35 BPM
  float on[N] = {};
  int w = 0;
  float period = 25.f, phase = 0.f, beats = 0.f;
  float conf = 0.f;                  // 0..1, smoothed peak of the normalised autocorrelation
  float onMean = 0.f, onVar = 0.f;
  float o1 = 0.f, o2 = 0.f, dens = 0.f;  // onset peaks per second (~4 s average)
  bool dbl = false;                      // the tempo found is every other beat: the music is twice as fast

  // onset: spectral flux of the frame (>= 0); quiet: frames that must not count (the robot itself talks)
  void feed(float onset, bool quiet = false) {
    float o = quiet ? 0.f : onset;
    on[w % N] = o;
    w++;
    bool peak = o1 > o2 && o1 >= o && o1 > onMean + 1.5f * sqrtf(onVar);
    dens += ((peak ? 50.f : 0.f) - dens) * 0.005f;
    o2 = o1;
    o1 = o;
    onMean += (o - onMean) * 0.02f;
    onVar += ((o - onMean) * (o - onMean) - onVar) * 0.02f;
    // beat clock, pulled towards strong onsets (a phase locked loop)
    phase += 1.f;
    if (phase >= period) {
      phase -= period;
      beats += 1.f;
    }
    if (o > onMean + 2.f * sqrtf(onVar) && conf > 0.2f) {
      float err = phase < period * 0.5f ? -phase : period - phase;  // onset late / early relative to the clock
      phase += 0.25f * err;
      if (phase < 0.f) {
        phase += period;
        beats -= 1.f;
      } else if (phase >= period) {
        phase -= period;
        beats += 1.f;
      }
    }
    if (w % 25 == 0 && w >= N) tempo();
  }
  void tempo() {
    float m = 0;
    for (int i = 0; i < N; ++i) m += on[i];
    m /= N;
    float r0 = 0, best = 0;
    int bl = 0;
    for (int i = 0; i < N; ++i) r0 += (on[i] - m) * (on[i] - m);
    if (r0 < 1e-6f) {
      conf *= 0.7f;
      return;
    }
    float rl[LMAX + 1] = {};
    for (int L = LMIN; L <= LMAX; ++L) {
      float r = 0;
      for (int k = 0; k < N - L; ++k) {
        int a = (w - 1 - k) % N, b = (w - 1 - k - L) % N;
        r += (on[a] - m) * (on[b] - m);
      }
      rl[L] = r = r / r0 * N / (N - L);
      if (r > best) best = r, bl = L;
    }
    // every multiple of the beat period correlates as well (2, 3 beats ...): the shortest peak that is nearly
    // as strong is the beat
    for (int L = LMIN + 1; L < bl; ++L)
      if (rl[L] >= 0.75f * best && rl[L] >= rl[L - 1] && rl[L] >= rl[L + 1]) {
        bl = L;
        break;
      }
    if (bl) period += ((float)bl - period) * (fabsf(bl - period) > 4.f ? 0.6f : 0.25f);
    // the music stopped: the last second has far fewer onsets than the window, let go quickly
    float m1 = 0;
    for (int k = 0; k < 50; ++k) m1 += on[(w - 1 - k) % N];
    if (m1 / 50.f < 0.45f * m) best = 0;
    conf += (fminf(1.f, best * 1.6f) - conf) * 0.35f;
    halfCheck();
  }
  // through small speakers often only the snare on every other beat stands out, but the hi-hats and the kick
  // between them still make many more onsets than beats: then the music is twice the tempo found
  void halfCheck() {
    float ratio = dens / (3000.f / period / 60.f);  // onsets per found beat
    if (3000.f / period >= 90.f) dbl = false;
    else if (ratio > 3.2f) dbl = true;
    else if (ratio < 2.4f) dbl = false;
  }
  float bpm() const { return 3000.f / period * (dbl ? 2.f : 1.f); }
  // how driving the music is, 0 calm .. 1 fast and busy: from the tempo and from how many onsets there are
  // (also when only every other beat was found as the tempo)
  float drive() const { return fminf(1.f, fmaxf(0.f, fmaxf((bpm() - 75.f) / 65.f, (dens - 1.2f) / 3.6f))); }
  float position() const { return (beats + phase / period) * (dbl ? 2.f : 1.f); }  // continuous beat count
};
