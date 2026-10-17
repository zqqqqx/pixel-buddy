// "Pixel" wake word inference. Mirrors wakeword/kwsfeat.py (features) and wakeword/train.py
// (network, BatchNorm and input normalisation already folded into the weights).
#include "kws.h"

#include <math.h>
#include <string.h>

#include "kws_model.h"
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#define BIG EXT_RAM_BSS_ATTR  // activations in PSRAM: internal RAM is kept for WiFi / TLS
#else
#define BIG
#endif


namespace kws {

static const int SR = 16000, WIN = 480, HOP = 320, NFFT = 512, NBIN = NFFT / 2 + 1;
static const int T1 = KWS_FRAMES, T2 = (KWS_FRAMES - 1) / 2 + 1, T3 = (T2 - 1) / 2 + 1;  // 49, 25, 13
static const float REFRACTORY_S = 1.5f;

BIG static float hist[WIN];          // last 480 samples
static int fill = 0;             // new samples since the last frame
BIG static float win[WIN];
BIG static float re[NFFT], im[NFFT];
BIG static float cosT[NFFT / 2], sinT[NFFT / 2];
BIG static short rev[NFFT];
BIG static float feat[KWS_FRAMES][KWS_NMEL];  // ring of log-mel frames
static int fHead = 0, frames = 0;
static float prob = 0, prevProb = 0, thr = KWS_THRESHOLD;
static int sinceHit = 1 << 20;   // frames since the last detection

void init() {
  for (int n = 0; n < WIN; ++n) win[n] = 0.5f - 0.5f * cosf(2.f * (float)M_PI * n / WIN);
  for (int k = 0; k < NFFT / 2; ++k) {
    cosT[k] = cosf(2.f * (float)M_PI * k / NFFT);
    sinT[k] = -sinf(2.f * (float)M_PI * k / NFFT);
  }
  for (int i = 0; i < NFFT; ++i) {
    int r = 0;
    for (int b = 0; b < 9; ++b) r |= ((i >> b) & 1) << (8 - b);
    rev[i] = (short)r;
  }
  memset(hist, 0, sizeof(hist));
  fill = 0;
  frames = fHead = 0;
  prob = prevProb = 0;
}

static void fft() {  // in place, radix 2, re/im already bit reversed
  for (int len = 2; len <= NFFT; len <<= 1) {
    int half = len >> 1, step = NFFT / len;
    for (int i = 0; i < NFFT; i += len)
      for (int j = 0; j < half; ++j) {
        float wr = cosT[j * step], wi = sinT[j * step];
        int a = i + j, b = a + half;
        float xr = re[b] * wr - im[b] * wi, xi = re[b] * wi + im[b] * wr;
        re[b] = re[a] - xr;
        im[b] = im[a] - xi;
        re[a] += xr;
        im[a] += xi;
      }
  }
}

static void frame() {
  float mean = 0;
  for (int n = 0; n < WIN; ++n) mean += hist[n];
  mean /= WIN;
  for (int i = 0; i < NFFT; ++i) {
    int s = rev[i];
    re[i] = s < WIN ? (hist[s] - mean) * win[s] : 0.f;
    im[i] = 0.f;
  }
  fft();
  float* out = feat[fHead];
  const float* w = KWS_MEL_W;
  for (int m = 0; m < KWS_NMEL; ++m) {
    float acc = 0;
    for (int k = 0, b = KWS_MEL_START[m]; k < KWS_MEL_LEN[m]; ++k, ++b) acc += w[k] * (re[b] * re[b] + im[b] * im[b]);
    w += KWS_MEL_LEN[m];
    out[m] = logf(acc + 1e-6f);
  }
  fHead = (fHead + 1) % KWS_FRAMES;
  if (frames < KWS_FRAMES) ++frames;
}

// ---- network (channel-major activations [C][T])
BIG static float x0[KWS_NMEL][T1];
BIG static float a1[KWS_C1][T1], d2[KWS_C1][T2], a2[KWS_C2][T2], d3[KWS_C2][T3], a3[KWS_C3][T3],
    d4[KWS_C3][T3], a4[KWS_C3][T3];

static inline float relu(float v) { return v > 0.f ? v : 0.f; }

// depthwise, kernel 5, padding 2
static void dwconv(const float* in, int C, int Tin, float* out, int Tout, int stride, const float* W, const float* B) {
  for (int c = 0; c < C; ++c) {
    const float* x = in + c * Tin;
    const float* w = W + c * 5;
    for (int t = 0; t < Tout; ++t) {
      float acc = B[c];
      int base = t * stride - 2;
      for (int k = 0; k < 5; ++k) {
        int i = base + k;
        if (i >= 0 && i < Tin) acc += w[k] * x[i];
      }
      out[c * Tout + t] = relu(acc);
    }
  }
}
static void pwconv(const float* in, int Cin, int T, float* out, int Cout, const float* W, const float* B) {
  for (int o = 0; o < Cout; ++o) {
    float* y = out + o * T;
    for (int t = 0; t < T; ++t) y[t] = B[o];
    for (int i = 0; i < Cin; ++i) {
      float w = W[o * Cin + i];
      const float* x = in + i * T;
      for (int t = 0; t < T; ++t) y[t] += w * x[t];
    }
    for (int t = 0; t < T; ++t) y[t] = relu(y[t]);
  }
}

static float infer() {
  // oldest frame first
  for (int t = 0; t < T1; ++t) {
    const float* f = feat[(fHead + t) % KWS_FRAMES];
    for (int m = 0; m < KWS_NMEL; ++m) x0[m][t] = f[m];
  }
  // conv 40 -> C1, kernel 3, padding 1
  for (int o = 0; o < KWS_C1; ++o) {
    float* y = a1[o];
    for (int t = 0; t < T1; ++t) y[t] = KWS_B1[o];
    for (int i = 0; i < KWS_NMEL; ++i) {
      const float* w = KWS_W1 + (o * KWS_NMEL + i) * 3;
      const float* x = x0[i];
      const float pad = KWS_PAD[i];  // padding = band mean (the input normalisation is folded into the weights)
      y[0] += w[0] * pad + w[1] * x[0] + w[2] * x[1];
      for (int t = 1; t < T1 - 1; ++t) y[t] += w[0] * x[t - 1] + w[1] * x[t] + w[2] * x[t + 1];
      y[T1 - 1] += w[0] * x[T1 - 2] + w[1] * x[T1 - 1] + w[2] * pad;
    }
    for (int t = 0; t < T1; ++t) y[t] = relu(y[t]);
  }
  dwconv(&a1[0][0], KWS_C1, T1, &d2[0][0], T2, 2, KWS_DW2, KWS_DB2);
  pwconv(&d2[0][0], KWS_C1, T2, &a2[0][0], KWS_C2, KWS_PW2, KWS_PB2);
  dwconv(&a2[0][0], KWS_C2, T2, &d3[0][0], T3, 2, KWS_DW3, KWS_DB3);
  pwconv(&d3[0][0], KWS_C2, T3, &a3[0][0], KWS_C3, KWS_PW3, KWS_PB3);
  dwconv(&a3[0][0], KWS_C3, T3, &d4[0][0], T3, 1, KWS_DW4, KWS_DB4);
  pwconv(&d4[0][0], KWS_C3, T3, &a4[0][0], KWS_C3, KWS_PW4, KWS_PB4);
  float g[KWS_C3], h[32];
  for (int c = 0; c < KWS_C3; ++c) {
    float s = 0;
    for (int t = 0; t < T3; ++t) s += a4[c][t];
    g[c] = s / T3;
  }
  for (int o = 0; o < 32; ++o) {
    float s = KWS_FB1[o];
    for (int i = 0; i < KWS_C3; ++i) s += KWS_FC1[o * KWS_C3 + i] * g[i];
    h[o] = relu(s);
  }
  float z = KWS_FB2[0];
  for (int i = 0; i < 32; ++i) z += KWS_FC2[i] * h[i];
  return 1.f / (1.f + expf(-z));
}

bool feed(const int16_t* x, int n, bool detect) {
  bool hit = false;
  for (int i = 0; i < n; ++i) {
    // shift in one sample at a time would be slow: collect HOP samples at the end of hist
    hist[WIN - HOP + fill] = x[i] * (1.f / 32768.f);
    if (++fill < HOP) continue;
    fill = 0;
    frame();
    memmove(hist, hist + HOP, (WIN - HOP) * sizeof(float));
    ++sinceHit;
    if (detect && frames == KWS_FRAMES && sinceHit % KWS_EVAL_EVERY == 0) {
      prevProb = prob;
      prob = infer();
      if ((prob + prevProb) * 0.5f > thr && sinceHit > (int)(REFRACTORY_S * SR / HOP)) {
        sinceHit = 0;
        hit = true;
      }
    }
  }
  return hit;
}

static_assert(BANDS == KWS_NMEL, "kws.h BANDS");
float lastProb() { return prob; }
const float* lastFrame() { return feat[(fHead + KWS_FRAMES - 1) % KWS_FRAMES]; }

void setSensitivity(float s) {
  if (s < 0) s = 0;
  if (s > 1) s = 1;
  // in logit space: 0.5 = trained threshold, every 0.1 step moves it by 0.8 (s = 1: ~0.65, s = 0: ~0.9998)
  float l = logf(KWS_THRESHOLD / (1.f - KWS_THRESHOLD)) + (0.5f - s) * 8.f;
  thr = 1.f / (1.f + expf(-l));
}

}  // namespace kws
