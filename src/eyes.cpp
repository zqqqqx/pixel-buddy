// Procedural cartoon eyes: SDF renderer + spring-driven animation rig.
//
// Every frame the animation layer produces a "pose" (targets for ~40 channels),
// springs chase those targets (giving overshoot, squash and follow-through),
// and the result is baked into a Frame snapshot of analytic shapes:
//   sclera  = egg-shaped ellipse  ∩ upper lid curve ∩ lower lid curve
//             morphing into a round-capped stroke when closed, or into > < chevrons
//   iris    = egg-shaped ellipse with motion stretch, foreshortening,
//             morphing into a heart or a star, clipped by the sclera
// The renderer evaluates signed distance fields per pixel with 1px anti-aliasing.
#include "eyes.h"
#include "config.h"

#include <math.h>
#include <string.h>

// hot per-pixel code runs from IRAM on the ESP32 (no flash cache misses while both cores render)
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#define HOT IRAM_ATTR
#else
#define HOT
#endif

namespace eyes {

// ============================================================== helpers
static const float PI = 3.14159265f, TAU = 6.28318531f;
static inline float clampf(float x, float a, float b) { return x < a ? a : (x > b ? b : x); }
static inline float sat(float x) { return clampf(x, 0.f, 1.f); }
static inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
static inline float maxf(float a, float b) { return a > b ? a : b; }
static inline float minf(float a, float b) { return a < b ? a : b; }
static inline float sstep(float a, float b, float x) {
  float t = sat((x - a) / (b - a));
  return t * t * (3.f - 2.f * t);
}
static inline float ffloor(float x) {
  int i = (int)x;
  return (float)(x < (float)i ? i - 1 : i);
}
// The ESP32-S3 FPU has no single-instruction divide / sqrt, the compiler emits
// library calls for them. In the per-pixel code we use bit-trick estimates
// refined by Newton steps instead (relative error < 1e-5).
static inline float frsqrt(float x) {  // 1/sqrt(x), x > 0
  union {
    float f;
    uint32_t i;
  } u = {x};
  u.i = 0x5f375a86u - (u.i >> 1);
  float y = u.f, hx = 0.5f * x;
  return y * (1.5f - hx * y * y);
}
static inline float fsqrt(float x) { return x > 1e-12f ? x * frsqrt(x) : 0.f; }
static inline float frcp(float x) {  // 1/x, x > 0
  union {
    float f;
    uint32_t i;
  } u = {x};
  u.i = 0x7ef311c3u - u.i;
  float y = u.f;
  y = y * (2.f - x * y);
  y = y * (2.f - x * y);
  y = y * (2.f - x * y);
  return y;
}
// fast sine (radians), max error ~0.001
static inline float fsin(float x) {
  float t = x * 0.15915494f;
  t -= ffloor(t + 0.5f);
  float y = 8.f * t - 16.f * t * fabsf(t);
  return 0.225f * (y * fabsf(y) - y) + y;
}
static inline float fcos(float x) { return fsin(x + 1.5707963f); }
static inline float fatan2(float y, float x) {
  float ax = fabsf(x), ay = fabsf(y);
  float mn = minf(ax, ay), mx = maxf(ax, ay);
  float a = mn * frcp(mx + 1e-12f);
  float s = a * a;
  float r = ((-0.0464964749f * s + 0.15931422f) * s - 0.327622764f) * s * a + a;
  if (ay > ax) r = 1.57079637f - r;
  if (x < 0) r = PI - r;
  if (y < 0) r = -r;
  return r;
}
// bell-shaped bump centred at c with half width w
static inline float bump(float t, float c, float w) {
  float u = (t - c) / w;
  if (u * u >= 1.f) return 0.f;
  u = 1.f - u * u;
  return u * u;
}
static inline float env(float t, float a, float b, float c, float d) {
  return sstep(a, b, t) * (1.f - sstep(c, d, t));
}

static uint32_t rs = 0x9e3779b9u;
static inline uint32_t rnd() {
  rs ^= rs << 13;
  rs ^= rs >> 17;
  rs ^= rs << 5;
  return rs;
}
static inline float frand() { return (rnd() >> 8) * (1.f / 16777216.f); }
static inline float frand(float a, float b) { return a + (b - a) * frand(); }

// ============================================================== SDFs
// approximate distance to an axis aligned ellipse (inverse radii given)
static inline float sdEllipse(float px, float py, float irx, float iry) {
  float ux = px * irx, uy = py * iry;
  float vx = ux * irx, vy = uy * iry;
  float q1 = vx * vx + vy * vy;
  if (q1 < 1e-14f) return -frcp(maxf(irx, iry));
  float k0 = fsqrt(ux * ux + uy * uy);
  return k0 * (k0 - 1.f) * frsqrt(q1);
}
// egg: different top / bottom radii (given as inverses), width varying with height
static inline float sdEgg(float px, float py, float irx, float iryT, float iryB, float egg) {
  float iry = py < 0.f ? iryT : iryB;
  float t = clampf(py * iry, -1.f, 1.f) * egg;
  return sdEllipse(px, py, irx * (1.f - t + t * t), iry);
}
// segment with precomputed b-a and 1/|b-a|^2
static inline float sdSegK(float px, float py, float ax, float ay, float bax, float bay, float ibb) {
  float pax = px - ax, pay = py - ay;
  float h = sat((pax * bax + pay * bay) * ibb);
  float dx = pax - bax * h, dy = pay - bay * h;
  return fsqrt(dx * dx + dy * dy);
}
static inline float sdSeg(float px, float py, float ax, float ay, float bx, float by) {
  float bax = bx - ax, bay = by - ay;
  return sdSegK(px, py, ax, ay, bax, bay, frcp(bax * bax + bay * bay));
}
// heart, y up, spans x[-0.6,0.6] y[0,1.1]
HOT static inline float sdHeart(float x, float y) {
  x = fabsf(x);
  if (y + x > 1.f) {
    float dx = x - 0.25f, dy = y - 0.75f;
    return fsqrt(dx * dx + dy * dy) - 0.35355339f;
  }
  float ay = y - 1.f;
  float a = x * x + ay * ay;
  float m = 0.5f * maxf(x + y, 0.f);
  float bx = x - m, by = y - m;
  float b = bx * bx + by * by;
  return fsqrt(minf(a, b)) * (x > y ? 1.f : -1.f);
}
// n-pointed star (iq), r outer radius; precomputed sector constants
struct StarK {
  float an, acx, acy, ecx, ecy, iper, hmax;
};
static StarK starK(int n, float m) {
  StarK k;
  k.an = PI / n;
  float en = PI / m;
  k.acx = cosf(k.an);
  k.acy = sinf(k.an);
  k.ecx = cosf(en);
  k.ecy = sinf(en);
  k.iper = 1.f / (2.f * k.an);
  k.hmax = k.acy / k.ecy;
  return k;
}
static StarK STAR5, STAR4;
HOT static inline float sdStar(float px, float py, float r, const StarK& k) {
  float a = fatan2(px, py) + 4.f * PI;
  float per = 2.f * k.an;
  float bn = a - ffloor(a * k.iper) * per - k.an;
  float l = fsqrt(px * px + py * py);
  float qx = l * fcos(bn), qy = l * fabsf(fsin(bn));
  qx -= r * k.acx;
  qy -= r * k.acy;
  float h = clampf(-(qx * k.ecx + qy * k.ecy), 0.f, r * k.hmax);
  qx += k.ecx * h;
  qy += k.ecy * h;
  float d = fsqrt(qx * qx + qy * qy);
  return qx < 0 ? -d : d;
}

// rounded box, half size hx hy, corner radius r
static inline float sdBox(float px, float py, float hx, float hy, float r) {
  float qx = fabsf(px) - hx + r, qy = fabsf(py) - hy + r;
  float ox = maxf(qx, 0.f), oy = maxf(qy, 0.f);
  return fsqrt(ox * ox + oy * oy) + minf(maxf(qx, qy), 0.f) - r;
}
// the face shape (Look.shape 0..1): 0 egg, 0.3 pill, 1 rounded square. k = egg -> pill blend, r = corner
// radius as a fraction of the shorter half side. The web interface draws its pictures with the same numbers.
static inline float shapeK(float s) { return s < 0.3f ? s * (1.f / 0.3f) : 1.f; }
static inline float shapeR(float s, float squarest) { return s < 0.3f ? 1.f : 1.f - (1.f - squarest) * (s - 0.3f) * (1.f / 0.7f); }
// arc of radius R opening upwards (+y), half aperture given by its sine / cosine (iq sdArc)
static inline float sdArcUp(float px, float py, float s, float c, float R) {
  px = fabsf(px);
  if (c * px > s * py) {
    float dx = px - s * R, dy = py - c * R;
    return fsqrt(dx * dx + dy * dy);
  }
  return fabsf(fsqrt(px * px + py * py) - R);
}
// question mark, v down, about +-0.9
static inline float sdQues(float u, float v) {
  float cx = u, cy = v + 0.38f;
  float r = fsqrt(cx * cx + cy * cy);
  float da;
  if (cx < 0 && cy > 0) {
    float ex = cx + 0.4f, ey = cy;
    da = fsqrt(ex * ex + ey * ey);
  } else {
    da = fabsf(r - 0.4f);
  }
  float d = minf(da, sdSeg(u, v, 0.f, 0.02f, 0.f, 0.3f)) - 0.15f;
  float dx = u, dy = v - 0.72f;
  return minf(d, fsqrt(dx * dx + dy * dy) - 0.17f);
}

// ============================================================== colours
struct RGB {
  float r, g, b;
};
static inline RGB mix(const RGB& a, const RGB& b, float t) {
  return {lerpf(a.r, b.r, t), lerpf(a.g, b.g, t), lerpf(a.b, b.b, t)};
}
struct Colorway {
  const char* name;
  RGB sclera, iris, accent, fx, hl;
};
static const Colorway CW[] = {
    {"porcelain", {246, 243, 234}, {38, 92, 255}, {255, 104, 156}, {246, 243, 234}, {255, 255, 255}},
    {"gumball", {255, 38, 24}, {255, 255, 255}, {255, 140, 180}, {255, 255, 255}, {255, 255, 255}},
    {"grape soda", {148, 88, 255}, {255, 226, 72}, {255, 128, 208}, {255, 226, 72}, {255, 255, 255}},
    {"marmalade", {255, 118, 22}, {255, 216, 92}, {255, 96, 120}, {255, 216, 92}, {255, 255, 255}},
    {"mint", {70, 232, 176}, {14, 34, 58}, {255, 130, 170}, {70, 232, 176}, {255, 255, 255}},
    {"bubblegum", {255, 102, 178}, {255, 255, 255}, {255, 190, 225}, {255, 255, 255}, {255, 255, 255}},
    {"lemon", {255, 226, 76}, {24, 24, 30}, {255, 128, 96}, {255, 226, 76}, {255, 255, 255}},
    {"ocean", {40, 128, 255}, {236, 248, 255}, {255, 130, 170}, {236, 248, 255}, {255, 255, 255}},
    {"classic", {244, 244, 244}, {16, 16, 20}, {255, 110, 150}, {244, 244, 244}, {255, 255, 255}},
    {"lime", {160, 255, 64}, {22, 22, 26}, {255, 120, 170}, {160, 255, 64}, {255, 255, 255}},
};
static const int NCW = sizeof(CW) / sizeof(CW[0]);
static int cwCur = CFG_COLORWAY, cwPrev = CFG_COLORWAY;
static Colorway cwCustom = {"custom", {246, 243, 234}, {38, 92, 255}, {255, 104, 156}, {246, 243, 234}, {255, 255, 255}};
static RGB bgCol = {0, 0, 0};
static inline const Colorway& cwAt(int i) { return i >= 0 && i < NCW ? CW[i] : cwCustom; }
static float cwT = 1.f;
static const RGB TEAR_COL = {120, 200, 255};
static const RGB ANGER_COL = {255, 64, 64};

// ============================================================== rig
// geometry (set from the Look, see setLook)
static const float CX = 120.f;
static float CY = 118.f;
static float SEP = 57.f;                 // half distance between eye centres
static float RX = 50.f, RYT = 57.f, RYB = 55.f;
static float BASE_TILT = 0.085f;         // eyes lean outwards a little
static float IRX = 0.47f, IRYT = 0.80f, IRYB = 0.76f, IEGG = 0.10f;
static float STROKE = 6.5f;              // closed eye line thickness
static Look L;

struct Spring {
  float x = 0, v = 0, k = 100, c = 10;
  void tune(float f, float z) {
    float w = TAU * f;
    k = w * w;
    c = 2.f * z * w;
  }
  void step(float target, float dt) {
    int n = (int)(dt / 0.004f) + 1;
    float h = dt / n;
    for (int i = 0; i < n; ++i) {
      v += (k * (target - x) - c * v) * h;
      x += v * h;
    }
  }
};

// per eye channels
enum { LT, LB, ST, SB, SX, SY, OX, OY, TILT, IS, IA, HEART, STAR, CHEV, HL, GXO, GYO, GLY, PU, NE };
// global channels
enum { GX, GY, FX, FY, FT, BLUSH, NG };
static const float EF[NE] = {9, 7, 6, 6, 4.2f, 4.2f, 4, 4, 3.2f, 4.5f, 4, 3.2f, 3.2f, 4.5f, 3, 6, 6, 4.f, 3.5f};
static const float EZ[NE] = {.85f, .8f, .8f, .8f, .3f, .3f, .5f, .5f, .45f, .38f, .5f, .7f, .7f, .75f, .8f, .55f, .55f, .5f, .45f};
static float ENEU[NE] = {0, 0, 0, 0, 1, 1, 0, 0, 0, 1, 1, 0, 0, 0, CFG_HIGHLIGHTS, 0, 0, 0, 1};
static const float GF[NG] = {6.5f, 6.5f, 2.0f, 2.4f, 2.2f, 1.6f};
static const float GZ[NG] = {.6f, .6f, .62f, .42f, .5f, 1.f};

struct Pose {
  float e[2][NE];
  float g[NG];
  float dx, dy, drot;  // direct (unsprung) offsets: shakes, hops
  float wob;           // sustained jelly wobble
  float glitch;        // horizontal slice tearing (error "glitch")
};
static Pose P;
static Spring SE[2][NE], SG[NG];
// wandering over the screen: x, y offset (px) and zoom (closer / farther). Idle picks its own spots,
// a few behaviours steer it (roamTo), everything else brings the face back to the middle.
static Spring SR[3];
static float roamTX = 0, roamTY = 0, roamTZ = 1, roamTimer = 3.f;
static bool roamHeld = false;

static float T = 0;              // global time
static float wobA = 0;           // decaying wobble impulse
static float prevDy = 0, vyS = 0, prevDx = 0, vxS = 0;
static float starRot = 0;
static bool blinkSuppress = false;

// Symbols the iris can morph into (error animations). The behaviour sets the type and
// its parameters every frame; the per-eye GLY channel springs the morph in and out.
// from GL_BULB on the accent part has its own colour (glyphCol)
enum { GL_NONE, GL_WIFI, GL_BATT, GL_QUES, GL_NOPE, GL_LOCK, GL_BULB, GL_CLOCK, GL_WEATHER, GL_GAUGE };
static int glyphType = GL_NONE;
static RGB glyphCol = {255, 196, 110};  // the accent of the coloured symbols (from GL_BULB on, and the battery)
static float gp[4] = {0, 0, 0, 0};  // WIFI: arc 1, arc 2 | BATT: level, empty blink | LOCK: shackle lift | BULB: glow, rays
                                    // CLOCK: minute / hour hand angle, bells | WEATHER: phase, -, condition
static float glyphRot = 0;
static float glitchAmt = 0, glitchSeedT = 0;
static uint32_t glitchSeed = 1;

// ============================================================== frame snapshot
struct EyeR {
  float cx, cy, c, s;
  float rx, ryT, ryB, egg, irx2;  // irx2 = 1/rx^2 for lid derivative
  float sirx, siryT, siryB;        // inverse sclera radii
  float yT, yB, kT, kB;
  bool lidT, lidB;
  float closeK, c0, c1, c2, L, hT;
  float chev, chevDir, chW, chH, chT, chBx, chBy, chIbb;
  float ix, iy, m00, m01, m10, m11, irx, iryT, iryB, iegg, ibR, irisA;
  float iirx, iiryT, iiryB;  // inverse iris radii
  float heart, hs, ihs, star, sr, starC, starS;
  float hl, h1x, h1y, h1r, h2x, h2y, h2r;
  float glyph, gs, igs, grc, grs;  // symbol morph amount, scale, rotation
  float gcx, gcy;                  // symbol centre
  float sqK, sqR, isqR, pr;        // ellipse -> pill blend, corner radius factor (sclera / iris), pupil radius (px)
  float lcT;                       // top lid curvature (1 = follows the egg, 0 = straight like a shutter)
  bool flT, flB;                   // the resting face's straight lids (look: lid_top / lid_bottom / brow)
  float fT, fTk, fTn, fB;          // top lid y at the centre, slope, 1/sqrt(1+slope^2); bottom lid y
  float wobA, wobPh;
  float Ms, Mi;  // block classification margins
  Rect bb;
};
enum { SP_HEART, SP_Z, SP_SPARK, SP_EXCL, SP_QUES, SP_TEAR, SP_ANGER, SP_WAVE, SP_DOT };
struct SprR {
  int type;
  float x, y, sc, c, s, a;
  RGB col;
  Rect bb;
};
struct BlushR {
  float cx, cy, rx, ry, a;
  Rect bb;
};
static const int MAXS = 18;
struct Frame {
  EyeR eye[2];
  BlushR blush[2];
  SprR spr[MAXS];
  int nspr;
  RGB scl, iris, acc, hl, fx, bg;
  RGB gcol;             // accent colour of the symbols from GL_BULB on (lamp colour, weather ...)
  RGB pup;              // pupil colour (a dark iris)
  int glyph;            // GL_* symbol shown in the irises
  float gp[4];
  float tr[20];         // sin / cos for the symbols, once per frame: clock hands 0-3, sun rays 4-19
  float glitch;         // slice tearing amount in px
  uint32_t glitchSeed;
  Rect dirty;
};
static Frame F;
static Rect prevBox = {0, 0, W, H};

// ============================================================== sprites
struct Spr {
  bool alive;
  int type, col;  // col: 0 accent, 1 fx, 2 white, 3 tear, 4 anger
  float x, y, vx, vy, ay, size, grow, rot, vrot, age, life, wig, wigF, ph;
};
static Spr spr[MAXS];

static void spawn(int type, int col, float x, float y, float vx, float vy, float size, float life,
                  float wig = 0, float rot = 0, float vrot = 0, float grow = 0, float ay = 0) {
  for (auto& s : spr) {
    if (s.alive) continue;
    s = {true, type, col, x, y, vx, vy, ay, size, grow, rot, vrot, 0, life, wig, frand(1.5f, 2.6f), frand(0, TAU)};
    return;
  }
}
static void sparkleAt(float x, float y, float size = 9) {
  spawn(SP_SPARK, 2, x, y, frand(-6, 6), frand(-14, -6), size, 0.9f, 0, frand(-0.3f, 0.3f), frand(-2, 2));
}
static void sparkleNearEye(int i) {
  const EyeR& e = F.eye[i];
  float sg = i == 0 ? -1.f : 1.f;
  sparkleAt(e.cx + sg * e.rx * frand(0.75f, 1.0f), e.cy - e.ryT * frand(0.7f, 1.0f), frand(7, 11));
}
static void spawnHeart(float x, float y) {
  spawn(SP_HEART, 0, x, y, frand(-8, 8), frand(-62, -44), frand(8, 13), frand(2.2f, 2.8f), frand(5, 10),
        frand(-0.25f, 0.25f));
}
static void spawnHeartAround() {
  float r = frand();
  float x = r < 0.5f ? CX + frand(-12, 12) : (r < 0.75f ? frand(8, 26) : frand(214, 232));
  spawnHeart(x, frand(200, 225));
}
static void updateSprites(float dt) {
  for (auto& s : spr) {
    if (!s.alive) continue;
    s.age += dt;
    if (s.age >= s.life) {
      s.alive = false;
      continue;
    }
    s.vy += s.ay * dt;
    s.x += s.vx * dt;
    s.y += s.vy * dt;
    s.rot += s.vrot * dt;
    s.size += s.grow * dt;
  }
}

// ============================================================== blink
struct Blink {
  bool on;
  float t, close, hold, open;
};
static Blink blink = {false, 0, 0, 0, 0};
static float nextBlink = 2.f;
static int blinkQueue = 0;
static float blinkVal[2] = {0, 0};

static void startBlink(float speed = 1.f) {
  if (blink.on) return;
  blink = {true, 0, 0.065f / speed, 0.035f / speed, 0.13f / speed};
}
static float blinkCurve(float t) {
  const Blink& b = blink;
  if (t <= 0) return 0;
  if (t < b.close) {
    float u = t / b.close;
    return u * u;
  }
  t -= b.close;
  if (t < b.hold) return 1;
  t -= b.hold;
  if (t < b.open) {
    float u = 1.f - t / b.open;
    return u * u * (1.5f - 0.5f * u);
  }
  return 0;
}
static void updateBlink(float dt) {
  if (blink.on) {
    float prev = blink.t;
    blink.t += dt;
    float openStart = blink.close + blink.hold;
    if (prev < openStart && blink.t >= openStart) {  // eyes pop open a touch
      for (int i = 0; i < 2; ++i) {
        SE[i][SY].v += 0.9f;
        SE[i][SX].v -= 0.4f;
      }
    }
    blinkVal[0] = blinkCurve(blink.t);
    blinkVal[1] = blinkCurve(blink.t - 0.014f);  // the right eye lags a hair
    if (blink.t > openStart + blink.open + 0.02f) {
      blink.on = false;
      blinkVal[0] = blinkVal[1] = 0;
      if (blinkQueue > 0) {
        blinkQueue--;
        nextBlink = 0.09f;
      }
    }
    return;
  }
  nextBlink -= dt;
  if (nextBlink <= 0) {
    bool closed = SE[0][LT].x > 0.75f || SE[0][LB].x > 0.6f || SE[0][CHEV].x > 0.3f;
    if (!blinkSuppress && !closed) {
      float r = frand();
      if (r < 0.06f) {
        startBlink(0.5f);  // slow, content blink
      } else {
        startBlink(frand(0.9f, 1.15f));
        if (r > 0.82f) blinkQueue = 1;  // double blink
      }
    }
    nextBlink = frand(L.blinkMin, maxf(L.blinkMin, L.blinkMax));
  }
}

// ============================================================== behaviours
static float bt = 0, bpt = 0;  // behaviour time / previous
static int side = 1;
static inline bool at(float x) { return bpt < x && bt >= x; }
static inline bool every(float p, float off) {
  if (bt < off) return false;
  if (bpt < off) return true;
  return (int)((bt - off) / p) != (int)((bpt - off) / p);
}
static inline void E(int ch, float v, float w) {
  P.e[0][ch] = lerpf(P.e[0][ch], v, w);
  P.e[1][ch] = lerpf(P.e[1][ch], v, w);
}
static inline void E1(int i, int ch, float v, float w) { P.e[i][ch] = lerpf(P.e[i][ch], v, w); }
static inline void G(int ch, float v, float w) { P.g[ch] = lerpf(P.g[ch], v, w); }
static inline void GA(int ch, float v) { P.g[ch] += v; }
static inline void kickE(int ch, float v) {
  SE[0][ch].v += v;
  SE[1][ch].v += v;
}
static inline void wobKick(float a) { wobA = maxf(wobA, a); }
static inline float hop(float t, float f) { return fabsf(fsin(PI * f * t)); }
static float touchGX = 0, touchGY = 0;
static float lookX = 0, lookY = 0;
static void nextColorway() {
  cwPrev = cwCur;
  cwCur = (cwCur < 0 || cwCur >= NCW - 1) ? 0 : cwCur + 1;
  cwT = 0;
}

static void bHappy(float t, float D, float w) {
  E(LB, 0.96f, w);
  E(PU, 1.2f, w);
  E(SY, 1.04f, w);
  float h = (t > 0.25f && t < D - 0.45f) ? hop(t - 0.25f, 3.8f) : 0.f;
  P.dy -= 7.f * h * w;
  GA(FT, 0.07f * fsin(TAU * 0.95f * t) * w);
  G(BLUSH, 1, w);
  G(GX, 0, w * 0.7f);
  G(GY, -0.15f, w * 0.7f);
  if (at(0.35f)) sparkleNearEye(0);
  if (at(0.85f)) sparkleNearEye(1);
}

static void bLove(float t, float D, float w) {
  E(HEART, 1, w);
  E(PU, 1.5f, w);
  float ph = t - ffloor(t / 0.9f) * 0.9f;
  float beat = bump(ph, 0.1f, 0.1f) + 0.65f * bump(ph, 0.3f, 0.1f);
  E(IS, 1.12f + 0.2f * beat, w);
  E(SX, 1.f + 0.035f * beat, w);
  E(SY, 1.f + 0.035f * beat, w);
  G(BLUSH, 0.9f, w);
  G(GX, 0, w);
  G(GY, 0.05f, w);
  if (t > 0.3f && t < D - 0.9f && every(0.42f, 0.3f)) spawnHeartAround();
}

static void bSurprised(float t, float D, float w) {
  float pre = 1.f - sstep(0.06f, 0.14f, t);
  E(PU, 0.6f, w);  // tiny pupils: shock
  E(SY, lerpf(1.18f, 0.86f, pre), w);
  E(SX, lerpf(1.13f, 1.06f, pre), w);
  E(IS, lerpf(0.45f, 1.f, pre), w);
  E(LT, 0, w);
  E(LB, 0, w);
  P.dy -= 9.f * bump(t, 0.3f, 0.2f) * w;
  if (t < 1.2f) P.dx += 0.9f * fsin(TAU * 21.f * t) * (1.f - t / 1.2f) * w;
  G(GX, 0, w);
  G(GY, -0.05f, w);
  if (at(0.14f)) spawn(SP_EXCL, 1, CX + 72, 34, 0, -6, 13, 1.3f, 0, 0.15f);
  if (at(D - 0.5f)) startBlink();
}

static void bSleepy(float t, float D, float w) {
  const float wake = 7.6f;
  blinkSuppress = t < wake + 0.6f;
  if (t < wake) {
    float lt;
    if (t < 3.2f)
      lt = 0.38f + 0.32f * sstep(0, 3.2f, t) - 0.3f * bump(t, 1.35f, 0.28f) - 0.32f * bump(t, 2.5f, 0.25f);
    else
      lt = lerpf(0.7f, 1.f, sstep(3.2f, 3.9f, t));
    E(LT, lt, w);
    G(GY, 0.35f, w);
    G(GX, 0, w);
    float deep = sstep(3.f, 4.f, t);
    GA(FT, (0.1f * sstep(2.5f, 4.f, t) + 0.04f * fsin(TAU * 0.25f * t)) * w);
    P.dy += (4.f + 2.5f * fsin(TAU * 0.28f * t)) * deep * w;
    E(SY, 1.f + 0.03f * fsin(TAU * 0.28f * t) * deep, w);
    if (t > 3.9f && every(1.15f, 3.9f)) {
      const EyeR& e = F.eye[1];
      spawn(SP_Z, 1, e.cx + 22, e.cy - 34, 16, -24, 6, 2.4f, 5, -0.25f, 0, 4.5f);
    }
  } else {
    float u = t - wake;
    float pop = w * (1.f - sstep(0.7f, 1.5f, u));
    E(LT, 0, w);
    E(SX, 1.12f, pop);
    E(SY, 1.16f, pop);
    E(IS, 0.55f, pop);
    if (at(wake)) {
      kickE(SY, 4.f);
      spawn(SP_EXCL, 1, CX + 74, 34, 0, -6, 13, 1.0f, 0, 0.15f);
    }
    P.dy -= 10.f * bump(u, 0.18f, 0.2f) * w;
    if (u > 0.3f) P.dx += 7.f * fsin(TAU * 5.5f * (u - 0.3f)) * expf(-3.f * (u - 0.3f)) * w;
    float gx = (u > 0.55f && u < 0.95f) ? -0.85f : (u >= 0.95f && u < 1.35f) ? 0.85f : 0.f;
    G(GX, gx, w);
    G(GY, 0, w);
  }
}

static void bDizzy(float t, float D, float w) {
  float a = TAU * 1.5f * t;
  float r = 0.62f * sstep(0, 0.5f, t) * (1.f - sstep(D - 1.f, D - 0.6f, t));
  G(GX, 0, w);
  G(GY, 0, w);
  E1(0, GXO, r * fcos(a), w);
  E1(0, GYO, r * fsin(a), w);
  E1(1, GXO, r * fcos(-a + PI), w);
  E1(1, GYO, r * fsin(-a + PI), w);
  P.wob = maxf(P.wob, 2.2f * (r / 0.62f) * w);
  GA(FT, 0.14f * fsin(TAU * 0.55f * t) * w);
  GA(FX, 6.f * fsin(TAU * 0.55f * t + 1.f) * w);
  E(LT, 0.18f, w);
  float u = t - (D - 0.9f);
  if (u > 0) P.dx += 6.f * fsin(TAU * 7.f * u) * expf(-4.f * u) * w;
  if (at(D - 0.35f)) startBlink();
}

static void bCurious(float t, float D, float w) {
  float s = (float)side;
  E(PU, 1.3f, w);
  GA(FT, 0.2f * s * w);
  G(GX, 0.55f * s, w);
  G(GY, -0.3f, w);
  int up = s > 0 ? 0 : 1;  // positive tilt raises the left eye
  E1(up, SX, 1.1f, w);
  E1(up, SY, 1.12f, w);
  E1(1 - up, LT, 0.32f, w);
  E1(1 - up, SX, 0.95f, w);
  if (at(0.55f)) {
    const EyeR& e = F.eye[up];
    spawn(SP_QUES, 1, e.cx + (up == 0 ? -12.f : 12.f), e.cy - e.ryT - 10, 0, -10, 12, 1.9f, 3, 0.2f * s);
  }
  if (at(2.3f)) startBlink();
}

static void bWink(float t, float D, float w) {
  blinkSuppress = true;
  int k = side > 0 ? 0 : 1;
  float w2 = env(t, 0.08f, 0.2f, D - 0.55f, D - 0.3f);
  E1(k, LB, 0.96f, w2);
  E1(1 - k, LB, 0.28f, w);
  E1(1 - k, SX, 1.05f, w);
  E1(1 - k, SY, 1.05f, w);
  GA(FT, (k == 0 ? 0.08f : -0.08f) * w);
  P.dy -= 3.f * bump(t, 0.25f, 0.2f) * w;
  G(GX, 0, w * 0.8f);
  G(GY, 0, w * 0.8f);
  if (at(0.22f)) {
    const EyeR& e = F.eye[k];
    float sg = k == 0 ? -1.f : 1.f;
    sparkleAt(e.cx + sg * e.rx * 0.9f, e.cy - e.ryT * 0.6f, 12);
  }
}

static void bGrumpy(float t, float D, float w) {
  E(ST, 0.85f, w);
  E(PU, 0.7f, w);
  E(LT, 0.3f, w);
  E(IS, 0.62f, w);
  E1(0, OX, 3, w);
  E1(1, OX, -3, w);
  E(SY, 0.94f, w);
  if (t < 1.4f) P.dx += 1.3f * fsin(TAU * 23.f * t) * w;
  float turn = env(t, 1.5f, 1.8f, 2.5f, 2.8f);
  G(GX, -0.85f * side * turn, w);
  G(GY, 0.1f, w);
  GA(FT, -0.07f * side * turn * w);
  if (at(0.25f)) {
    const EyeR& e = F.eye[side > 0 ? 1 : 0];
    spawn(SP_ANGER, 4, e.cx + (side > 0 ? 30.f : -30.f), e.cy - e.ryT - 4, 0, 0, 14, 1.8f, 0, 0, 0);
  }
}

static void bSad(float t, float D, float w) {
  E(ST, -0.7f, w);
  E(PU, 1.3f, w);
  E(LT, 0.18f, w);
  E(IS, 1.22f, w);
  E(HL, 1, w);
  G(GX, 0, w);
  G(GY, -0.45f, w);
  GA(FY, 5.f * w);
  E(SY, 1.03f, w);
  float tr = 0.03f * fsin(TAU * 8.f * t) * w;
  P.e[0][GYO] += tr;
  P.e[1][GYO] += tr;
  P.wob = maxf(P.wob, 0.6f * w);
  if (at(1.0f)) {
    const EyeR& e = F.eye[0];
    spawn(SP_TEAR, 3, e.cx - e.rx * 0.62f, e.cy + e.ryB * 0.55f, 0, 6, 9.f, 1.7f, 0, 0, 0, 0, 70);
  }
  if (at(2.0f)) {
    const EyeR& e = F.eye[1];
    spawn(SP_TEAR, 3, e.cx + e.rx * 0.62f, e.cy + e.ryB * 0.55f, 0, 6, 8.f, 1.5f, 0, 0, 0, 0, 70);
  }
}

static void bSuspicious(float t, float D, float w) {
  float take = sstep(2.9f, 3.05f, t);
  E(LT, lerpf(0.52f, 0.f, take), w);
  E(LB, lerpf(0.34f, 0.f, take), w);
  E(ST, lerpf(0.2f, 0.f, take), w);
  float pan = t < 1.1f ? -0.85f : lerpf(-0.85f, 0.85f, sstep(1.1f, 2.5f, t));
  G(GX, lerpf(pan * side, 0.f, take), w);
  G(GY, 0.05f, w);
  E(SX, lerpf(1.f, 1.08f, take), w);
  E(SY, lerpf(1.f, 1.1f, take), w);
  if (at(2.95f)) kickE(SY, 3.f);
  if (at(3.5f)) startBlink();
}

static void bGiggle(float t, float D, float w) {
  E(CHEV, 1, w);
  float h = (t > 0.2f && t < D - 0.4f) ? hop(t, 3.4f) : 0.f;
  P.dy -= 5.f * h * w;
  GA(FT, 0.07f * fsin(TAU * 1.7f * t) * w);
  G(BLUSH, 0.8f, w);
  G(GX, 0, w);
  G(GY, 0, w);
}

static void bJelly(float t, float D, float w) {
  float pre = env(t, 0.f, 0.12f, 0.2f, 0.26f);
  E(SY, lerpf(1.f, 0.8f, pre), 1);
  E(SX, lerpf(1.f, 1.12f, pre), 1);
  if (at(0.24f)) {
    kickE(SY, 5.5f);
    kickE(SX, -4.f);
    wobKick(5.f);
  }
  P.dy -= 8.f * bump(t, 0.42f, 0.2f);
}

static void bStarstruck(float t, float D, float w) {
  E(STAR, 1, w);
  E(IS, 1.3f, w);
  starRot = t * 1.4f;
  float h = (t > 0.3f && t < D - 0.5f) ? hop(t, 2.5f) : 0.f;
  P.dy -= 6.f * h * w;
  G(GX, 0, w);
  G(GY, -0.05f, w);
  if (t < D - 0.6f && every(0.4f, 0.3f)) sparkleNearEye(rnd() & 1);
}

static void bCrossEyed(float t, float D, float w) {
  float c = env(t, 0.1f, 0.45f, D - 0.8f, D - 0.5f);
  E1(0, GXO, 0.85f, c);
  E1(1, GXO, -0.85f, c);
  G(GX, 0, w);
  G(GY, 0.2f, w);
  E(LT, 0.12f, c);
  P.wob = maxf(P.wob, 1.f * c);
  float u = t - (D - 0.75f);
  if (u > 0) P.dx += 5.f * fsin(TAU * 7.f * u) * expf(-4.f * u);
  if (at(D - 0.3f)) startBlink();
}

static void bEyeRoll(float t, float D, float w) {
  float u = sstep(0.25f, 1.5f, t);
  float th = PI * (1.f - u);
  float back = sstep(1.6f, 1.9f, t);
  G(GX, lerpf(0.82f * fcos(th), 0.f, back), w);
  G(GY, lerpf(-0.78f * fsin(th), 0.15f, back), w);
  E(LT, lerpf(0.3f, 0.48f, back), w);
  GA(FT, -0.05f * back * w);
  if (at(2.0f)) startBlink(0.6f);
}

static void bShy(float t, float D, float w) {
  float peek = env(t, 1.9f, 2.05f, 2.45f, 2.65f);
  G(GX, lerpf(-0.75f * side, 0.05f, peek), w);
  G(GY, lerpf(0.65f, 0.05f, peek), w);
  E(LT, lerpf(0.34f, 0.12f, peek), w);
  E(SX, 0.94f, w);
  E(SY, 0.94f, w);
  E(HL, 1, w);
  G(BLUSH, 1, w);
  GA(FT, -0.07f * side * w);
  P.dx += 1.5f * fsin(TAU * 1.1f * t) * w;
}

static void bYawn(float t, float D, float w) {
  float st = env(t, 0.f, 0.6f, 0.7f, 0.95f);
  float sq = env(t, 0.75f, 1.f, 1.9f, 2.25f);
  E(SY, 1.f + 0.12f * st - 0.08f * sq, w);
  E(SX, 1.f - 0.06f * st + 0.06f * sq, w);
  P.dy -= 6.f * st * w;
  E(CHEV, sq, w);
  G(GY, -0.5f * st, w);
  if (sq > 0.5f) P.dx += 0.8f * fsin(TAU * 13.f * t) * w;
  float sl = sstep(2.1f, 2.4f, t);
  E(LT, 0.5f * sl * (1.f - sstep(D - 0.6f, D, t)), w);
  blinkSuppress = t < 2.4f;
  if (at(2.9f)) startBlink(0.55f);
}

static void bLookAround(float t, float D, float w) {
  if (every(0.38f, 0.f)) {
    lookX = frand(-0.95f, 0.95f);
    lookY = frand(-0.6f, 0.6f);
  }
  G(GX, lookX, w);
  G(GY, lookY, w);
  E(IS, 0.92f, w);
  E(SX, 1.04f, w);
  E(SY, 1.04f, w);
}

static void bColor(float t, float D, float w) {
  blinkSuppress = true;
  if (at(0.01f)) startBlink(0.45f);
  if (at(0.16f)) nextColorway();
  if (at(0.3f)) {
    wobKick(2.5f);
    sparkleNearEye(0);
    sparkleNearEye(1);
  }
  P.dy -= 4.f * bump(t, 0.3f, 0.2f);
}

static void bBoop(float t, float D, float w) {
  if (at(0.001f)) {
    kickE(SY, -5.f);
    kickE(SX, 4.f);
    wobKick(3.f);
  }
  E(LB, 0.62f, env(t, 0.15f, 0.3f, D - 0.4f, D - 0.15f));
  G(BLUSH, 0.6f, w);
  G(GX, touchGX, w);
  G(GY, touchGY, w);
  P.dy -= 4.f * bump(t, 0.25f, 0.18f);
}

static void bPet(float t, float D, float w) {
  E(LB, 0.95f, w);
  G(BLUSH, 1, w);
  GA(FT, 0.09f * fsin(TAU * 0.7f * t) * w);
  P.dy += 2.f * fsin(TAU * 0.7f * t) * w;
  blinkSuppress = true;
  if (every(0.6f, 0.4f)) spawnHeartAround();
}

static void bWake(float t, float D, float w) {
  blinkSuppress = true;
  // asleep -> heavy lids -> slow blink -> pop open -> look around
  float lt = 1.f;
  if (t > 0.9f) lt = lerpf(1.f, 0.55f, sstep(0.9f, 1.5f, t));
  if (t > 1.8f) lt = lerpf(lt, 1.f, bump(t, 2.05f, 0.25f));
  if (t > 2.35f) lt = lerpf(lt, 0.f, sstep(2.35f, 2.5f, t));
  E(LT, lt, 1);
  G(GY, lerpf(0.3f, 0.f, sstep(2.3f, 2.6f, t)), 1);
  float look = (t > 2.8f && t < 3.25f) ? -0.6f : (t >= 3.25f && t < 3.7f) ? 0.6f : 0.f;
  G(GX, look, w);
  E(SY, 1.f + 0.06f * bump(t, 2.55f, 0.25f), 1);
  if (at(2.42f)) kickE(SY, 3.f);
  P.dy += 4.f * (1.f - sstep(2.2f, 2.6f, t)) - 5.f * bump(t, 2.6f, 0.18f);
  GA(FT, 0.06f * (1.f - sstep(2.2f, 2.6f, t)));
}

// ---- voice: listening and thinking
// mood while listening, chosen live from what the person says (happy news -> smile, ...)
enum { MOOD_NONE, MOOD_HAPPY, MOOD_SAD, MOOD_SURPRISED, MOOD_LOVE, MOOD_CURIOUS, MOOD_FUNNY, MOOD_WORRIED };
static int moodK = MOOD_NONE;
static float moodS = 0, moodSetT = -100;
static bool vActive = false, vOnsetNow = false;
static float vLevelIn = 0, vPitchIn = 0.5f, vLevel = 0, vLevelSlow = 0, vPitch = 0.5f, vOnsetT = -1, vQuiet = 9;
static float earSide = 1;
static bool wakeReq = false;

static void spawnWaves(float strength) {
  // sound-wave arcs ")))" radiating from the upper outer corner of the raised eye
  int k = earSide > 0 ? 0 : 1;
  const EyeR& e = F.eye[k];
  float sg = k == 0 ? -1.f : 1.f;
  float ang = k == 0 ? -2.36f : -0.785f;  // up-left / up-right
  float dx = fcos(ang), dy = fsin(ang);
  float x = e.cx + sg * e.rx * 0.62f + dx * 8.f, y = e.cy - e.ryT * 0.62f + dy * 8.f;
  float sz = 10.f + 7.f * strength;
  spawn(SP_WAVE, 1, x, y, dx * 36.f, dy * 36.f, sz, 0.8f, 0, ang, 0, 30.f);
  spawn(SP_WAVE, 1, x - dx * 5.f, y - dy * 5.f, dx * 30.f, dy * 30.f, sz * 0.55f, 0.65f, 0, ang, 0, 24.f);
}

static void bListen(float t, float D, float w) {
  blinkSuppress = true;
  E(PU, 1.15f, w);  // attentive
  if (every(4.3f, 2.2f)) startBlink(0.7f);  // slow, attentive blinks only
  // perk up: anticipation squash, then pop open
  float pre = env(t, 0.f, 0.07f, 0.09f, 0.16f);
  if (at(0.12f)) {
    kickE(SY, 4.5f);
    kickE(SX, -2.f);
    spawnWaves(0.6f);
  }
  float L = vLevel;
  // dog-like head tilt that slowly changes side
  if (every(2.9f, 2.9f)) earSide = -earSide;
  GA(FT, 0.15f * earSide * w);
  int up = earSide > 0 ? 0 : 1;
  E1(up, SX, 1.1f, w);
  E1(up, SY, 1.1f, w);
  E(SY, lerpf(1.05f, 0.86f, pre), w);
  E(SX, lerpf(1.04f, 1.08f, pre), w);
  // irises breathe with loudness, their shape follows the pitch
  E(IS, 1.04f + 0.24f * L, w);
  E(IA, 0.94f + 0.18f * vPitch, w);
  E(HL, 0.85f, w);
  E(LT, 0.05f, w);
  E(LB, 0.1f + 0.26f * L, w);
  // look at the speaker, hopping between their eyes
  float hopEye = ((int)(t / 0.7f) & 1) ? 0.07f : -0.07f;
  G(GX, hopEye, w);
  G(GY, -0.1f, w);
  P.dy += 2.f * w;  // lean in
  P.wob = maxf(P.wob, 0.8f * L * w);
  // the mood of what is being said colours the listening face
  float m = moodS * w;
  if (m > 0.01f) {
    switch (moodK) {
      case MOOD_HAPPY:
        for (int i = 0; i < 2; ++i) P.e[i][LB] += 0.36f * m;
        G(BLUSH, 0.7f, m);
        P.dy -= 2.5f * hop(t, 2.4f) * m;
        break;
      case MOOD_SAD:
        E(ST, -0.6f, m);
        E(LT, 0.2f, m);
        E(IS, 1.22f, m);
        break;
      case MOOD_SURPRISED:
        E(SX, 1.12f, m);
        E(SY, 1.16f, m);
        E(IS, 0.72f, m);
        E(LT, 0.f, m);
        E(LB, 0.f, m);
        break;
      case MOOD_LOVE:
        E(HEART, 0.9f, m);
        G(BLUSH, 0.9f, m);
        break;
      case MOOD_CURIOUS:
        GA(FT, 0.1f * earSide * m);
        E1(up, SY, 1.18f, m);
        E1(1 - up, LT, 0.28f, m);
        break;
      case MOOD_FUNNY:
        for (int i = 0; i < 2; ++i) P.e[i][LB] += 0.45f * m;
        P.dy -= 3.f * hop(t, 3.4f) * m;
        G(BLUSH, 0.5f, m);
        break;
      case MOOD_WORRIED:
        E(ST, -0.45f, m);
        E(IS, 0.68f, m);
        P.dx += 0.8f * fsin(TAU * 16.f * t) * m;
        break;
    }
  }
  // every syllable: a small nod and a ripple
  if (vOnsetNow && t > 0.3f) {
    SG[FY].v += 30.f + 40.f * L;
    kickE(SY, 1.f + 2.5f * L);
    kickE(IS, 1.2f * L);
    spawnWaves(L);
  }
}

// a voice without the wake word: prick up, glance towards it, settle back
static void bHear(float t, float D, float w) {
  float s = (float)side;
  if (at(0.02f)) {
    kickE(SY, 2.5f);
    earSide = s > 0 ? -1.f : 1.f;  // waves on the side we look to
    spawnWaves(0.3f);
  }
  float look = env(t, 0.05f, 0.18f, D - 0.6f, D - 0.25f);
  G(GX, 0.7f * s, look);
  G(GY, -0.15f, look);
  E(SX, 1.04f, w);
  E(SY, 1.06f, w);
  E(IS, 1.08f, w);
  GA(FT, 0.05f * s * w);
}

// Waiting for the answer (open ended while thinkWait, otherwise a short "hmm").
// Eyes look up and slowly circle like a loading spinner, the side alternates,
// a chain of dots ripples above the head.
static bool thinkWait = false;
static int dotK = 0;
static void bThink(float t, float D, float w) {
  float ph = ((int)(t / 2.6f) & 1) ? -(float)side : (float)side;
  G(GX, 0.45f * ph + 0.12f * fcos(TAU * 0.7f * t), w);
  G(GY, -0.58f + 0.1f * fsin(TAU * 0.7f * t), w);
  E(LT, 0.24f, w);
  E(IS, 0.86f, w);
  E1(ph > 0 ? 1 : 0, LB, 0.2f, w);  // one eye squints while thinking
  GA(FT, -0.08f * ph * w);
  P.dy += 1.5f * fsin(TAU * 0.35f * t) * w;
  if (every(0.3f, 0.15f)) {
    spawn(SP_DOT, 1, CX + 15.f * (dotK - 1), 28.f, 0, -4.f, 4.2f, 0.85f);
    dotK = (dotK + 1) % 3;
  }
}

// ---- answers
static void bYes(float t, float D, float w) {
  float nod = bump(t, 0.3f, 0.16f) + bump(t, 0.72f, 0.16f) + 0.6f * bump(t, 1.12f, 0.15f);
  P.dy += 10.f * nod * w;                    // head goes down ...
  G(GY, -0.12f - 0.3f * nod, w);             // ... while the eyes keep looking at you
  G(GX, 0, w);
  E(LB, 0.32f, w);
  E(SY, 1.03f - 0.08f * nod, w);
  E(SX, 1.f + 0.04f * nod, w);
  GA(FT, 0.04f * fsin(TAU * 1.2f * t) * w);
}

static void bNo(float t, float D, float w) {
  float sh = fsin(TAU * 2.4f * (t - 0.15f)) * env(t, 0.1f, 0.25f, 1.2f, 1.6f);
  P.dx += 11.f * sh * w;
  P.drot -= 0.05f * sh * w;
  G(GX, -0.4f * sh, w);  // the eyes lag behind the head
  G(GY, 0.05f, w);
  E(LT, 0.22f, w);
  E(ST, 0.25f, w);
  E(IS, 0.85f, w);
}

static void bDontKnow(float t, float D, float w) {
  float s = (float)side;
  float up = bump(t, 0.45f, 0.3f) + 0.7f * bump(t, 1.4f, 0.25f);  // shrug
  P.dy -= 8.f * up * w;
  E(ST, -0.5f, w);
  E(LT, 0.12f, w);
  E(SY, 1.f + 0.05f * up, w);
  GA(FT, 0.12f * s * sstep(0.2f, 0.6f, t) * w);
  G(GX, lerpf(0.5f * s, -0.5f * s, sstep(0.9f, 1.3f, t)), w);
  G(GY, -0.4f, w);
  int big = s > 0 ? 0 : 1;
  E1(big, SX, 1.08f, w);
  E1(big, SY, 1.08f, w);
  if (at(0.5f)) {
    const EyeR& e = F.eye[big];
    spawn(SP_QUES, 1, e.cx + (big == 0 ? -10.f : 10.f), e.cy - e.ryT - 8, 0, -10, 12, 1.6f, 3, 0.2f * s);
  }
}

static void bCry(float t, float D, float w) {
  blinkSuppress = true;
  E(ST, -0.8f, w);
  E(LT, 0.25f, w);
  E(LB, 0.35f, w);
  E(IS, 1.15f, w);
  E(HL, 1, w);
  G(GX, 0, w);
  G(GY, -0.2f, w);
  P.dy += (3.f + 1.2f * fsin(TAU * 7.f * t)) * w;  // sobbing
  E(SY, 1.f - 0.04f * fabsf(fsin(TAU * 1.6f * t)), w);
  P.wob = maxf(P.wob, 0.8f * w);
  if (t > 0.3f && t < D - 0.6f && every(0.33f, 0.3f)) {
    int k = dotK++ & 1;
    const EyeR& e = F.eye[k];
    float sg = k == 0 ? -1.f : 1.f;
    spawn(SP_TEAR, 3, e.cx + sg * e.rx * frand(0.35f, 0.7f), e.cy + e.ryB * 0.45f, sg * frand(5, 20), frand(0, 10),
          frand(6.f, 9.f), 1.2f, 0, 0, 0, 0, 90);
  }
}

static float dartX = 0;
static void bScared(float t, float D, float w) {
  E(IS, 0.42f, w);
  E(PU, 0.5f, w);
  E(SX, 1.08f, w);
  E(SY, 1.14f, w);
  E(ST, -0.3f, w);
  P.dx += 1.3f * fsin(TAU * 19.f * t) * w;  // shiver
  P.dy += 3.f * w;                           // shrinks down
  if (every(0.32f, 0.1f)) dartX = frand(-0.75f, 0.75f);
  G(GX, dartX, w);
  G(GY, 0.05f, w);
  if (at(0.25f)) {  // sweat drop
    const EyeR& e = F.eye[1];
    spawn(SP_TEAR, 3, e.cx + e.rx * 0.95f, e.cy - e.ryT * 0.75f, 4, 8, 7.f, 1.6f, 0, 0, 0, 0, 40);
  }
}

static void bProud(float t, float D, float w) {
  float s = (float)side;
  P.dy -= 5.f * w;        // chin up
  G(GY, 0.25f, w);        // looking down the nose
  G(GX, 0.1f * s, w);
  E(LT, 0.38f, w);
  E(LB, 0.22f, w);
  E(IS, 0.9f, w);
  GA(FT, 0.08f * s * w);
  P.dy += 2.f * bump(t, 1.2f, 0.15f);  // little "hmph"
  if (at(0.35f)) sparkleNearEye(s > 0 ? 0 : 1);
}

static void bHello(float t, float D, float w) {
  if (at(0.05f)) kickE(SY, 4.f);
  P.dy -= 6.f * hop(t, 2.5f) * env(t, 0.1f, 0.2f, 1.3f, 1.6f) * w;
  GA(FT, 0.13f * fsin(TAU * 1.8f * t) * env(t, 0.f, 0.2f, 1.4f, 1.7f) * w);
  E(LB, 0.55f, w);
  G(GX, 0, w);
  G(GY, -0.05f, w);
  E1(0, LB, 0.95f, env(t, 1.55f, 1.65f, 1.95f, 2.1f));  // closing wink
  if (at(0.3f)) sparkleNearEye(1);
  if (at(1.6f)) sparkleNearEye(0);
}

static void bDance(float t, float D, float w) {
  // groove at ~120 bpm: sway, bounce on every beat, eyes switch between ^^ and wide open
  const float bpm = 2.f;
  float beat = t * bpm;
  float sway = fsin(PI * beat);
  P.dx += 12.f * sway * w;
  P.drot += 0.12f * sway * w;
  P.dy -= 7.f * hop(t, bpm) * w;
  bool half = ((int)(beat / 2.f)) & 1;
  E(LB, half ? 0.92f : 0.25f, w);
  E(IS, 1.1f, w);
  G(GX, -0.35f * sway, w);
  G(GY, -0.1f, w);
  G(BLUSH, 0.5f, w);
  if (every(1.f / bpm, 0.f)) {
    kickE(SY, 2.f);
    sparkleNearEye(rnd() & 1);
  }
}

// ---- sleep
static void bFallAsleep(float t, float D, float w) { bSleepy(t, 9.6f, w); }  // droop part only
static void bSleep(float t, float D, float w) {
  blinkSuppress = true;
  E(LT, 1, 1);
  G(GY, 0.35f, 1);
  G(GX, 0, 1);
  P.dy += 4.f + 2.5f * fsin(TAU * 0.25f * t);
  E(SY, 1.f + 0.03f * fsin(TAU * 0.25f * t), 1);
  GA(FT, 0.1f + 0.03f * fsin(TAU * 0.11f * t));
  if (every(1.6f, 0.8f)) {
    const EyeR& e = F.eye[1];
    spawn(SP_Z, 1, e.cx + 22, e.cy - 30, 16, -22, 6, 2.6f, 5, -0.25f, 0, 4.5f);
  }
  if (every(7.3f, 5.f)) kickE(SY, 1.5f);  // dreaming
}

// ---- idle (the only things that happen on their own)
static float idleGX = 0, idleGY = 0;
static void bGlance(float t, float D, float w) {
  if (at(0.01f)) {
    idleGX = 0.8f * side;
    idleGY = frand(-0.5f, 0.4f);
  }
  float g = env(t, 0.f, 0.08f, D - 0.4f, D - 0.15f);
  G(GX, idleGX, g);
  G(GY, idleGY, g);
  E(LT, 0.1f, g);
  if (at(D - 0.3f) && frand() < 0.5f) startBlink();
}
static void bDrift(float t, float D, float w) {
  float s = (float)side;
  GA(FX, 7.f * s * w);
  GA(FT, 0.06f * s * w);
  P.dy += 2.f * fsin(PI * t / D) * w;
  G(GX, lerpf(-0.35f, 0.45f, sstep(0.f, D, t)) * s, 0.6f * w);
  G(GY, 0.1f * fsin(TAU * t / D), 0.6f * w);
}
static void bFocus(float t, float D, float w) {
  if (at(0.01f)) {
    idleGX = frand(-0.7f, 0.7f);
    idleGY = frand(-0.4f, 0.4f);
  }
  float sq = sstep(0.6f, 1.2f, t);
  G(GX, idleGX, w);
  G(GY, idleGY, w);
  E(IS, lerpf(1.f, 0.8f, sq), w);
  E(LT, 0.28f * sq, w);
  E(LB, 0.18f * sq, w);
  E(SX, 1.f + 0.04f * sq, w);
  E(SY, 1.f + 0.04f * sq, w);
  P.dy += 2.f * sq * w;  // lean in
  blinkSuppress = t < D - 0.5f;
  if (at(D - 0.4f)) startBlink();
}
static void bPursuit(float t, float D, float w) {
  blinkSuppress = true;  // following something that flies past
  float u = sstep(0.3f, D - 0.5f, t);
  float s = (float)side;
  G(GX, lerpf(-0.9f, 0.9f, u) * s, w);
  G(GY, -0.3f - 0.3f * fsin(PI * u) + 0.05f * fsin(TAU * 2.f * t), w);
  E(IS, 1.05f, w);
  GA(FT, 0.06f * s * (u - 0.5f) * w);
}


// ---- moving around the screen
static inline void roamTo(float x, float y, float z) {
  roamTX = x;
  roamTY = y;
  roamTZ = z;
  roamHeld = true;
}
// how far the face may go sideways / up and down at a zoom (a little beyond the edge when it loves to roam)
static float roamRangeX(float z) { return maxf(0.f, 120.f - (SEP + RX) * z) + 14.f * L.roam; }
static float roamRangeY(float z) { return maxf(0.f, minf(CY, H - CY) - RYT * z - 4.f); }
static float wTX = 0, wTY = 0, wTZ = 1;

// wander: a bigger move with anticipation - looks there first, squats, glides, overshoots, wobbles
static void bWander(float t, float D, float w) {
  if (at(0.001f)) {
    wTZ = 1.f - L.roam * frand(0.f, 0.3f);
    wTX = frand(-1.f, 1.f) * roamRangeX(wTZ);
    wTY = frand(-0.8f, 0.8f) * roamRangeY(wTZ);
    if (fabsf(wTX - SR[0].x) < 25.f) wTX = -wTX;  // worth the trip
  }
  float lead = clampf((wTX - SR[0].x) / 45.f, -1.f, 1.f);
  G(GX, lead * (1.f - sstep(1.1f, 1.6f, t)), w);
  G(GY, clampf((wTY - SR[1].x) / 45.f, -0.8f, 0.8f) * (1.f - sstep(1.1f, 1.6f, t)), w);
  float squat = bump(t, 0.3f, 0.14f), lift = bump(t, 0.55f, 0.16f);
  E(SY, 1.f - 0.13f * squat * L.squash + 0.07f * lift * L.squash, w);
  E(SX, 1.f + 0.09f * squat * L.squash - 0.04f * lift * L.squash, w);
  if (t > 0.42f) roamTo(wTX, wTY, wTZ);
  if (at(1.35f)) wobKick(1.6f * L.squash);
  if (at(D - 0.5f)) startBlink(0.9f);
}

// peek: slides to an edge until one eye is outside, looks back in a bit suspiciously, comes back
static void bPeek(float t, float D, float w) {
  float out = env(t, 0.3f, 1.f, D - 1.3f, D - 0.4f);
  roamTo(side * (120.f - SEP + 0.35f * RX) * out, -6.f * out, 1.f);
  G(GX, -side * (0.55f + 0.3f * fsin(TAU * 0.7f * t)) * out, w);
  G(GY, 0.1f * fsin(TAU * 0.5f * t), w);
  E(LT, 0.3f * out, w);
  E(LB, 0.12f * out, w);
  GA(FT, -0.06f * side * out * w);
  if (at(D * 0.5f)) startBlink(1.3f);
}

// come closer: as if it leans to the glass to see you better, big pupils, a double blink
static void bApproach(float t, float D, float w) {
  float c = env(t, 0.2f, 1.1f, D - 1.1f, D - 0.3f);
  roamTo(0.f, 4.f * c, 1.f + 0.22f * c);
  E(PU, 1.f + 0.7f * c, w);
  E(IS, 1.f + 0.1f * c, w);
  G(GX, 0.18f * fsin(TAU * 0.45f * t) * c, w);
  G(GY, -0.05f, w);
  GA(FT, 0.06f * fsin(TAU * 0.35f * t) * c * w);
  if (at(1.4f)) startBlink(1.1f);
  if (at(1.7f)) startBlink(1.1f);
}

// bounce: three hops to new spots, squashed on every landing
static float hopX[4];
static void bBounce(float t, float D, float w) {
  const float HOP = 0.62f;
  if (at(0.001f))
    for (auto& x : hopX) x = frand(-1.f, 1.f) * roamRangeX(0.85f);
  int k = (int)(t / HOP);
  float ph = t - k * HOP;
  bool hopping = k < 3;
  if (hopping) {
    roamTo(hopX[k], 0.f, 0.85f);
    P.dy -= 24.f * hop(ph, 1.f / HOP) * w;
    G(GX, clampf((hopX[k] - SR[0].x) / 40.f, -1.f, 1.f), w);
  }
  for (int j = 1; j <= 3; ++j)
    if (at(j * HOP)) {
      kickE(SY, -2.6f * L.squash);
      wobKick(1.2f * L.squash);
    }
  E(LB, 0.25f, w);
}

// squish: the eyes play with their shape, one squashes while the other stretches
static void bSquish(float t, float D, float w) {
  float a = fsin(TAU * 1.5f * t) * env(t, 0.2f, 0.5f, D - 0.6f, D - 0.2f) * (0.6f + 0.4f * L.squash);
  E1(0, SX, 1.f + 0.17f * a, w);
  E1(0, SY, 1.f - 0.17f * a, w);
  E1(1, SX, 1.f - 0.17f * a, w);
  E1(1, SY, 1.f + 0.17f * a, w);
  E(LB, 0.3f, w);
  G(BLUSH, 0.4f, w);
}

// sway: rocks gently from side to side, as if humming along to something
static float swayX0 = 0;
static void bSway(float t, float D, float w) {
  if (at(0.001f)) swayX0 = roamTX;
  float sw = fsin(TAU * 0.55f * t), on = env(t, 0.3f, 0.9f, D - 0.9f, D - 0.3f);
  roamTo(swayX0 + 16.f * sw * on, -3.f * fabsf(sw) * on, roamTZ);
  GA(FT, 0.1f * sw * on * w);
  G(GX, -0.3f * sw * on, w);
  E(LB, 0.28f * on, w);
}

// ---- errors: no error messages, a clear symbol instead. The irises morph into the symbol
// (the "core" that makes each one recognisable), the gesture around it has 3 variations.
static int errVar = 0;
static inline void showGlyph(int type, float a) {
  glyphType = type;
  glyphRot = 0.f;  // a behaviour that turns its symbol sets it after this
  E(GLY, 1.f, a);
}
static void sweatDrop(int k) {
  const EyeR& e = F.eye[k];
  float sg = k == 0 ? -1.f : 1.f;
  spawn(SP_TEAR, 3, e.cx + sg * e.rx * 0.95f, e.cy - e.ryT * 0.7f, sg * 4.f, 6.f, 7.f, 1.6f, 0, 0, 0, 0, 45);
}

// no WiFi: WiFi symbols whose arcs light up one after the other (searching) and then stay dark
static void bOffline(float t, float D, float w) {
  blinkSuppress = t < D - 1.2f;
  showGlyph(GL_WIFI, w * env(t, 0.12f, 0.4f, D - 0.6f, D - 0.25f));
  const float give = D - 1.7f;  // searching until here, then it gives up
  float on = 1.f - sstep(give, give + 0.3f, t);
  float f = t * 1.5f - ffloor(t * 1.5f);
  gp[0] = on * sstep(0.22f, 0.32f, f);
  gp[1] = on * sstep(0.52f, 0.62f, f);
  float sad = sstep(give, give + 0.4f, t);
  float srch = (1.f - sad) * w;
  if (errVar == 0) {         // look left and right
    G(GX, 0.65f * fsin(TAU * 0.55f * t), srch);
    G(GY, -0.15f, srch);
    GA(FT, 0.06f * fsin(TAU * 0.55f * t - 0.6f) * srch);
  } else if (errVar == 1) {  // stretch up for a better signal
    G(GX, 0.2f * fsin(TAU * 0.4f * t), srch);
    G(GY, -0.7f, srch);
    E(SY, 1.06f, srch);
    P.dy -= (6.f + 4.f * hop(t, 2.2f)) * sstep(0.2f, 0.5f, t) * srch;
  } else {                   // slow circular scan with the head turning like an antenna
    float a = TAU * 0.45f * t;
    G(GX, 0.55f * fcos(a), srch);
    G(GY, -0.3f + 0.3f * fsin(a), srch);
    GA(FT, 0.09f * fcos(a) * srch);
  }
  G(GX, 0.f, sad * w);
  G(GY, 0.3f, sad * w);
  E(ST, -0.5f, sad * w);
  E(LT, 0.22f, sad * w);
  P.dy += (3.f * sad + 3.f * bump(t, give + 0.35f, 0.3f)) * w;  // little sigh
  if (at(D - 0.9f)) startBlink(0.6f);
}

// usage limit reached: a usage meter whose needle runs into the red and stays there, heavy, sinking eyes
static void bQuota(float t, float D, float w) {
  blinkSuppress = true;
  showGlyph(GL_GAUGE, w * env(t, 0.12f, 0.4f, D - 0.6f, D - 0.25f));
  float full = 1.75f;  // needle angle from 12 o'clock: -1.75 empty, +1.75 deep in the red
  float a;
  if (errVar == 0)
    a = lerpf(-full, full, sstep(0.4f, 1.8f, t));                                       // climbs
  else if (errVar == 1)
    a = full;                                                                            // already there
  else
    a = t < 1.5f ? lerpf(-full, 0.3f, sstep(0.4f, 1.1f, t)) : lerpf(0.3f, full, sstep(1.5f, 1.8f, t));  // jumps
  float red = sstep(1.7f, 1.9f, t);
  a += 0.06f * fsin(TAU * 13.f * t) * red;  // trembles at the stop
  gp[0] = a;
  float f = t * 2.4f - ffloor(t * 2.4f);
  glyphCol = mix(RGB{255, 140, 60}, RGB{255, 40, 40}, red * (f < 0.55f ? 1.f : 0.6f));
  float tired = sstep(1.6f, 3.6f, t);
  E(LT, 0.28f + 0.3f * tired, w);
  E(SY, 1.f - 0.07f * tired, w);
  G(GX, 0.f, w);
  G(GY, 0.1f + 0.3f * tired, w);
  P.dy += 7.f * tired * w;
  if (errVar == 1) {
    GA(FT, 0.12f * tired * side * w);  // head slumps to one side
    P.wob = maxf(P.wob, 1.6f * tired * w);
  }
  if (errVar == 2) P.dx += 1.6f * fsin(TAU * 21.f * t) * bump(t, 1.85f, 0.25f) * w;  // the needle hits the stop
  if (at(D - 1.1f)) startBlink(0.4f);
}

// "can't do that": a little no-entry sign, apologetic brows, a sweat drop
static void bCant(float t, float D, float w) {
  showGlyph(GL_NOPE, w * env(t, 0.08f, 0.28f, D - 0.5f, D - 0.2f));
  if (at(0.14f)) {
    kickE(SY, 3.f);
    wobKick(2.f);
  }
  E(ST, -0.55f, w);
  E(LT, 0.12f, w);
  E(IS, 1.f, w);
  G(GY, 0.05f, w);
  glyphRot = 0.f;
  if (errVar == 0) {         // shakes the head
    float sh = fsin(TAU * 2.2f * (t - 0.3f)) * env(t, 0.3f, 0.45f, 1.6f, 1.9f);
    P.dx += 9.f * sh * w;
    P.drot -= 0.04f * sh * w;
    G(GX, -0.3f * sh, w);
    if (at(0.5f)) sweatDrop(1);
  } else if (errVar == 1) {  // embarrassed glance to the side
    float g = env(t, 0.5f, 0.75f, 2.3f, 2.6f);
    G(GX, -0.6f * side, g * w);
    G(GY, 0.35f, g * w);
    E(SX, 0.93f, w);
    E(SY, 0.93f, w);
    P.dx += 1.2f * fsin(TAU * 1.1f * t) * w;
    if (at(0.45f)) sweatDrop(side > 0 ? 0 : 1);
  } else {                   // the sign spins in, then a small shake
    glyphRot = (1.f - sstep(0.08f, 0.8f, t)) * TAU;
    float sh = fsin(TAU * 2.4f * (t - 1.f)) * env(t, 1.f, 1.1f, 1.7f, 1.95f);
    P.dx += 6.f * sh * w;
    G(GX, -0.2f * sh, w);
    if (at(1.f)) sweatDrop(0);
  }
}

// didn't understand: question marks in both eyes, head tilt, one eye bigger
static void bHuh(float t, float D, float w) {
  float s = (float)side;
  showGlyph(GL_QUES, w * env(t, 0.18f, 0.4f, D - 0.5f, D - 0.2f));
  if (at(0.02f)) {
    startBlink(1.3f);
    blinkQueue = 1;
  }
  GA(FT, 0.17f * s * sstep(0.15f, 0.5f, t) * w);
  int k = s > 0 ? 0 : 1;
  E1(k, SX, 1.1f, w);
  E1(k, SY, 1.12f, w);
  E1(1 - k, LT, 0.3f, w);
  if (errVar == 0) {         // the question marks wobble
    glyphRot = 0.16f * fsin(TAU * 0.9f * t);
    G(GX, 0.3f * s, w);
    G(GY, -0.25f, w);
  } else if (errVar == 1) {  // leans in, listening harder
    glyphRot = 0.f;
    P.dy += 3.f * w;
    G(GX, 0.15f * s, w);
    earSide = s;
    if (every(0.7f, 0.45f) && t < D - 0.8f) spawnWaves(0.5f);
  } else {                   // bouncy
    glyphRot = 0.22f * fsin(TAU * 2.f * t);
    P.dy -= 4.f * hop(t, 2.f) * env(t, 0.3f, 0.4f, D - 0.7f, D - 0.4f) * w;
    G(GY, -0.15f, w);
  }
}

// no access (API key rejected): a padlock that is tugged at but stays shut
static void bLocked(float t, float D, float w) {
  showGlyph(GL_LOCK, w * env(t, 0.12f, 0.35f, D - 0.5f, D - 0.2f));
  float tug = bump(t, 0.85f, 0.12f) + bump(t, 1.25f, 0.12f) + bump(t, 1.65f, 0.12f);
  gp[0] = 0.4f * tug;
  float sad = sstep(2.f, 2.4f, t);
  E(LT, 0.2f, w);
  E(ST, lerpf(0.3f, -0.5f, sad), w);
  G(GX, 0.f, w);
  if (errVar == 0) {         // tugs upwards
    P.dy -= 4.f * tug * w;
    G(GY, -0.1f, w);
  } else if (errVar == 1) {  // looks down at the lock, then shrugs
    G(GY, lerpf(0.45f, 0.f, sad), w);
    P.dy -= 8.f * bump(t, 2.6f, 0.3f) * w;
  } else {                   // rattles it
    P.dx += 3.f * fsin(TAU * 14.f * t) * env(t, 0.75f, 0.85f, 1.85f, 2.f) * w;
    G(GY, 0.1f, w);
  }
  P.dy += 3.f * sad * w;
  if (at(D - 0.8f)) startBlink(0.6f);
}

// ---- smart home: the irises become a light bulb that glows in the lamp's colour
static RGB bulbCol = {255, 196, 110}, bulbFrom = {255, 196, 110};
static RGB hueCol(float h) {
  h -= ffloor(h);
  return {255.f * sat(fabsf(h * 6.f - 3.f) - 1.f), 255.f * sat(2.f - fabsf(h * 6.f - 2.f)),
          255.f * sat(2.f - fabsf(h * 6.f - 4.f))};
}

// light on: the bulb clicks on with a burst of rays, the eyes open up in the new light
static void bLightOn(float t, float D, float w) {
  showGlyph(GL_BULB, w * env(t, 0.1f, 0.35f, D - 0.55f, D - 0.2f));
  const float click = 0.7f;
  float on = sstep(click, click + 0.08f, t);
  gp[0] = on;
  gp[1] = env(t, click, click + 0.12f, click + 0.9f, click + 1.3f);
  glyphCol = bulbCol;
  if (at(click)) {
    kickE(SY, 3.f);
    wobKick(1.5f);
  }
  E(LT, lerpf(0.3f, 0.f, on), w);  // squinting in the dark, then wide awake
  E(SY, 1.f + 0.05f * on, w);
  G(BLUSH, 0.6f * on, w);
  G(GX, 0.f, w);
  if (errVar == 0) {         // looks up at the lamp
    G(GY, lerpf(0.1f, -0.55f, on), w);
  } else if (errVar == 1) {  // a little hop of joy
    G(GY, -0.2f, w);
    P.dy -= 8.f * hop(t - click, 2.6f) * on * (1.f - sstep(click + 0.7f, click + 0.8f, t)) * w;
  } else {                   // happy wiggle
    G(GY, -0.2f, w);
    GA(FT, 0.08f * fsin(TAU * 1.5f * t) * on * w);
  }
  if (at(click + 0.15f)) sparkleNearEye(0);
  if (at(click + 0.45f)) sparkleNearEye(1);
  if (at(D - 0.7f)) startBlink(0.5f);
}

// light off: the glow goes out, the eyes get sleepy and cosy in the dark
static void bLightOff(float t, float D, float w) {
  showGlyph(GL_BULB, w * env(t, 0.1f, 0.35f, D - 0.55f, D - 0.2f));
  const float click = 0.8f;
  float off = sstep(click, click + 0.06f, t);
  gp[0] = 1.f - off;
  gp[1] = 0.f;
  glyphCol = bulbCol;
  if (at(click)) wobKick(1.2f);
  E(LT, 0.05f + 0.38f * off, w);
  E(SY, 1.f - 0.05f * off, w);
  G(GY, lerpf(-0.45f, 0.15f, off), w);  // looks at the lamp, then down
  if (errVar == 0) {         // a cosy sigh
    G(GX, 0.f, w);
    P.dy += (3.f * off + 3.f * bump(t, click + 0.5f, 0.3f)) * w;
  } else if (errVar == 1) {  // peeks around in the dark
    G(GX, 0.5f * fsin(TAU * 0.6f * (t - click)) * off, w);
  } else {                   // blinks twice to get used to the dark
    G(GX, 0.f, w);
    if (at(click + 0.35f) || at(click + 0.75f)) startBlink(1.4f);
  }
  if (at(D - 0.7f)) startBlink(0.5f);
}

// new colour: the glow runs through the rainbow and lands on the new colour
static void bLightColor(float t, float D, float w) {
  showGlyph(GL_BULB, w * env(t, 0.1f, 0.35f, D - 0.55f, D - 0.2f));
  gp[0] = 1.f;
  float sweep = bump(t, 1.f, 0.55f);
  glyphCol = mix(mix(bulbFrom, bulbCol, sstep(0.45f, 1.5f, t)), hueCol(t * 1.4f), sweep);
  float done = sstep(1.5f, 1.7f, t);
  gp[1] = env(t, 1.5f, 1.62f, 2.2f, 2.6f);
  E(IS, 1.f + 0.1f * sweep, w);
  E(LB, 0.3f * done, w);  // a little happy squint, the new colour stays visible
  G(BLUSH, 0.7f * done, w);
  G(GX, 0.f, w);
  G(GY, -0.35f, w);
  if (errVar == 0) {         // the head sways with the colours
    GA(FT, 0.1f * fsin(TAU * 0.8f * t) * sweep * w);
  } else if (errVar == 1) {  // hops when it lands
    P.dy -= 7.f * hop(t - 1.55f, 3.f) * env(t, 1.55f, 1.6f, 2.1f, 2.2f) * w;
  } else {                   // wobbles like a lava lamp
    P.wob = maxf(P.wob, 1.6f * sweep * w);
  }
  if (at(1.6f)) sparkleNearEye(0);
  if (at(1.8f)) sparkleNearEye(1);
  if (at(D - 0.7f)) startBlink(0.5f);
}

// ---- timer / alarm: the irises become a little clock
// timer set: the hands spin to the new time, then a nod ("got it")
static void bTimer(float t, float D, float w) {
  showGlyph(GL_CLOCK, w * env(t, 0.1f, 0.35f, D - 0.55f, D - 0.2f));
  float spin = 1.f - (1.f - sstep(0.3f, 1.6f, t)) * (1.f - sstep(0.3f, 1.6f, t));  // ease out
  gp[0] = 0.4f + spin * 2.f * TAU;
  gp[1] = 2.1f + spin * 0.6f * TAU;
  gp[2] = 0.f;
  glyphCol = {255, 200, 80};
  G(GX, 0.f, w);
  G(GY, -0.2f, w);
  E(SY, 1.03f, w);
  P.dy -= 5.f * bump(t, 1.95f, 0.18f) * w + 3.f * bump(t, 2.3f, 0.15f) * w;  // nod nod
  if (errVar == 1) GA(FT, 0.07f * fsin(TAU * 1.3f * t) * (1.f - spin) * w);
  if (errVar == 2 && at(1.7f)) sparkleNearEye(side > 0 ? 0 : 1);
  if (at(D - 0.7f)) startBlink(0.5f);
}

// alarm: a ringing alarm clock with bells, wide awake eyes, the head rattles along
static void bAlarm(float t, float D, float w) {
  showGlyph(GL_CLOCK, w * env(t, 0.08f, 0.25f, D - 0.4f, D - 0.15f));
  float ring = env(t, 0.2f, 0.3f, D - 0.6f, D - 0.4f);
  glyphRot = 0.22f * fsin(TAU * 11.f * t) * ring;
  gp[0] = 0.f;
  gp[1] = TAU * 7.f / 12.f;
  gp[2] = 1.f;
  glyphCol = {255, 90, 70};
  E(SX, 1.08f, w);
  E(SY, 1.1f, w);
  E(LT, -0.1f, w);
  G(GY, -0.15f, w);
  P.dx += 2.5f * fsin(TAU * 11.f * t + 1.f) * ring * w;
  if (errVar == 1) P.dy -= 5.f * hop(t, 2.f) * ring * w;
  if (errVar == 2) GA(FT, 0.06f * fsin(TAU * 1.7f * t) * ring * w);
  if (every(0.5f, 0.2f) && t < D - 0.6f) sparkleNearEye(rnd() & 1);
}

// ---- battery: the real charge in the irises (filling up, coloured by the level); charging: it keeps filling
static float musicIn = 0, musicS = 0, musicBpm = 120, musicPos = 0, musicTarget = 0, musicLoud = 0, musicDrive = 0;
static int battLevel = -1;
static bool battCharging = false;
static void bBattery(float t, float D, float w) {
  showGlyph(GL_BATT, w * env(t, 0.1f, 0.35f, D - 0.55f, D - 0.2f));
  float lvl = battLevel < 0 ? 0.5f : battLevel / 100.f;
  float fill = lvl * sstep(0.35f, 1.3f, t);
  if (battCharging && t > 1.3f) {  // charging: from the level to full, again and again
    float f = (t - 1.3f) * 0.9f;
    fill = lvl + (1.f - lvl) * (f - ffloor(f));
  }
  gp[0] = maxf(0.04f, fill);
  float f = t * 2.4f - ffloor(t * 2.4f);
  gp[1] = lvl < 0.15f && !battCharging && t > 1.3f && f > 0.55f ? 0.f : 1.f;  // nearly empty: blinks
  glyphCol = lvl > 0.5f ? RGB{80, 220, 110} : lvl > 0.2f ? RGB{255, 200, 50} : RGB{255, 60, 50};
  G(GX, 0.f, w);
  float good = battCharging ? 1.f : sat((lvl - 0.2f) * 2.f);
  G(GY, 0.15f - 0.3f * good, w);
  E(LB, 0.3f * good, w);  // full: content; empty: tired
  E(LT, 0.3f * (1.f - good), w);
  P.dy += 6.f * (1.f - good) * sstep(1.3f, 2.5f, t) * w;
  if (battCharging) P.dy -= 3.f * bump(t, 1.4f, 0.15f) * w;
  if (at(D - 0.7f)) startBlink(0.5f);
}

// ---- weather: a symbol in the irises, the face goes with it
enum { W_SUN, W_PARTLY, W_CLOUD, W_RAIN, W_SNOW, W_STORM, W_FOG, W_NIGHT };
static int weatherCond = W_SUN;
static void bWeather(float t, float D, float w) {
  showGlyph(GL_WEATHER, w * env(t, 0.1f, 0.35f, D - 0.55f, D - 0.2f));
  gp[0] = t;
  gp[2] = (float)weatherCond;
  static const RGB COL[] = {{255, 205, 40}, {255, 205, 40}, {200, 210, 225}, {80, 160, 255},
                            {140, 195, 255}, {255, 220, 40}, {200, 210, 225}, {255, 225, 120}};
  glyphCol = COL[weatherCond];
  G(GX, 0.f, w);
  switch (weatherCond) {
    case W_SUN:
    case W_PARTLY:  // happy squint, a little warmth on the cheeks
      E(LB, 0.4f, w);
      G(BLUSH, 0.7f, w);
      G(GY, -0.3f, w);
      if (errVar == 1) P.dy -= 5.f * hop(t, 2.2f) * env(t, 0.4f, 0.5f, D - 0.9f, D - 0.6f) * w;
      break;
    case W_RAIN:  // looks up at the rain, a bit glum
      E(ST, -0.4f, w);
      E(LT, 0.15f, w);
      G(GY, -0.5f, w);
      if (errVar == 2) P.dy += 3.f * w;
      break;
    case W_SNOW:  // excited
      E(SX, 1.06f, w);
      E(SY, 1.08f, w);
      G(GY, -0.4f, w);
      if (every(0.6f, 0.4f) && t < D - 0.8f) sparkleNearEye(rnd() & 1);
      break;
    case W_STORM:  // a little scared, trembling
      E(ST, -0.6f, w);
      E(SY, 1.06f, w);
      P.dx += 1.2f * fsin(TAU * 13.f * t) * w;
      break;
    case W_NIGHT:  // sleepy
      E(LT, 0.4f, w);
      G(GY, 0.1f, w);
      break;
    default:  // clouds, fog: unimpressed
      E(LT, 0.25f, w);
      G(GY, -0.1f, w);
      if (errVar == 0) G(GX, 0.3f * fsin(TAU * 0.4f * t), w);
  }
  if (at(D - 0.7f)) startBlink(0.5f);
}

// something broke (server / connection): the picture tears and flickers, then a "reboot"
static void bGlitch(float t, float D, float w) {
  blinkSuppress = true;
  float g;
  if (errVar == 2)
    g = 22.f * env(t, 0.15f, 0.2f, 0.6f, 0.75f);
  else
    g = 14.f * (bump(t, 0.3f, 0.15f) + 0.8f * bump(t, 0.85f, 0.12f) + bump(t, 1.4f, 0.18f));
  P.glitch = g * w;
  float jr = (float)((glitchSeed >> 8) & 255) * (1.f / 255.f);
  float gl = sat(g / 10.f);
  E(IS, lerpf(1.f, 0.55f + 0.9f * jr, gl), w);
  E(SX, lerpf(1.f, 0.9f + 0.25f * jr, gl), w);
  if (at(0.3f) || at(1.4f)) {
    sparkleNearEye(0);
    sparkleNearEye(1);
  }
  if (errVar == 0) {         // power off, reboot
    float off = env(t, 1.85f, 2.1f, 2.45f, 2.55f);
    E(LT, off, w);
    if (at(2.5f)) {
      kickE(SY, 4.f);
      wobKick(2.f);
    }
    G(GX, 0.f, w);
  } else if (errVar == 1) {  // left a bit dizzy
    float r = 0.5f * env(t, 1.6f, 2.f, D - 0.6f, D - 0.2f);
    float a = TAU * 1.4f * t;
    E1(0, GXO, r * fcos(a), w);
    E1(0, GYO, r * fsin(a), w);
    E1(1, GXO, r * fcos(-a + PI), w);
    E1(1, GYO, r * fsin(-a + PI), w);
    P.wob = maxf(P.wob, 1.5f * r * w);
  } else {                   // shakes it off like a wet dog
    float u = t - 0.9f;
    if (u > 0) {
      P.dx += 10.f * fsin(TAU * 6.f * u) * expf(-2.5f * u) * w;
      P.drot += 0.06f * fsin(TAU * 6.f * u + 0.5f) * expf(-2.5f * u) * w;
    }
  }
}

typedef void (*BehFn)(float, float, float);
struct Beh {
  const char* name;
  float dur, weight, attack, release;
  BehFn fn;
};
enum {
  B_HAPPY, B_LOVE, B_SURPRISED, B_SLEEPY, B_DIZZY, B_CURIOUS, B_WINK, B_GRUMPY, B_SAD, B_SUSPICIOUS,
  B_GIGGLE, B_JELLY, B_STAR, B_CROSS, B_ROLL, B_SHY, B_YAWN, B_LOOK, B_COLOR, B_BOOP, B_PET, B_WAKE, B_LISTEN, B_THINK, B_HEAR,
  B_YES, B_NO, B_IDK, B_CRY, B_SCARED, B_PROUD, B_HELLO, B_FALLASLEEP, B_SLEEP,
  B_GLANCE, B_DRIFT, B_FOCUS, B_PURSUIT, B_DANCE,
  B_OFFLINE, B_QUOTA, B_CANT, B_HUH, B_LOCKED, B_GLITCH,
  B_LIGHT_ON, B_LIGHT_OFF, B_LIGHT_COLOR, B_TIMER, B_ALARM, B_WEATHER,
  B_WANDER, B_PEEK, B_APPROACH, B_BOUNCE, B_SQUISH, B_SWAY, B_BATTERY, NB
};
static const Beh BEH[NB] = {
    {"happy", 2.8f, 0, .25f, .35f, bHappy},
    {"love", 4.2f, 0, .3f, .5f, bLove},
    {"surprised", 2.2f, 0, .06f, .5f, bSurprised},
    {"sleepy", 9.6f, 0, .6f, .4f, bSleepy},
    {"dizzy", 3.8f, 0, .3f, .3f, bDizzy},
    {"curious", 3.4f, 0, .35f, .45f, bCurious},
    {"wink", 1.7f, 0, .1f, .3f, bWink},
    {"grumpy", 3.2f, 0, .2f, .45f, bGrumpy},
    {"sad", 3.8f, 0, .4f, .5f, bSad},
    {"suspicious", 4.2f, 0, .35f, .4f, bSuspicious},
    {"giggle", 2.4f, 0, .15f, .3f, bGiggle},
    {"jelly", 1.8f, 0, .05f, .3f, bJelly},
    {"starstruck", 3.4f, 0, .3f, .45f, bStarstruck},
    {"cross-eyed", 2.4f, 0, .15f, .3f, bCrossEyed},
    {"eye roll", 2.6f, 0, .2f, .4f, bEyeRoll},
    {"shy", 3.8f, 0, .4f, .5f, bShy},
    {"yawn", 3.6f, 0, .3f, .5f, bYawn},
    {"look around", 2.0f, 6, .1f, .3f, bLookAround},
    {"color", 1.4f, CFG_AUTO_COLOR_CHANGE ? 1.6f : 0.f, .05f, .2f, bColor},
    {"boop", 1.4f, 0, .05f, .4f, bBoop},
    {"pet", 600.f, 0, .3f, .3f, bPet},
    {"wake up", 4.0f, 0, .01f, .4f, bWake},
    {"listening", 600.f, 0, .12f, .3f, bListen},
    {"thinking", 30.f, 0, .25f, .4f, bThink},
    {"hear", 1.7f, 0, .1f, .4f, bHear},
    {"yes", 1.7f, 0, .1f, .3f, bYes},
    {"no", 1.9f, 0, .1f, .3f, bNo},
    {"dont know", 2.4f, 0, .2f, .4f, bDontKnow},
    {"cry", 4.2f, 0, .3f, .5f, bCry},
    {"scared", 2.6f, 0, .08f, .4f, bScared},
    {"proud", 2.6f, 0, .3f, .4f, bProud},
    {"hello", 2.3f, 0, .1f, .3f, bHello},
    {"falling asleep", 7.4f, 0, .6f, .01f, bFallAsleep},
    {"asleep", 1e6f, 0, .5f, .3f, bSleep},
    {"glance", 1.5f, 8, .05f, .3f, bGlance},
    {"drift", 4.5f, 6, .8f, .9f, bDrift},
    {"focus", 3.2f, 4, .3f, .5f, bFocus},
    {"pursuit", 3.4f, 4, .3f, .4f, bPursuit},
    {"dance", 4.2f, 0, .3f, .4f, bDance},
    {"offline", 5.0f, 0, .2f, .4f, bOffline},
    {"quota", 5.2f, 0, .3f, .5f, bQuota},
    {"cant", 3.6f, 0, .12f, .4f, bCant},
    {"huh", 3.4f, 0, .15f, .4f, bHuh},
    {"locked", 4.2f, 0, .2f, .45f, bLocked},
    {"glitch", 3.4f, 0, .05f, .4f, bGlitch},
    {"light_on", 3.0f, 0, .1f, .4f, bLightOn},
    {"light_off", 3.0f, 0, .1f, .4f, bLightOff},
    {"light_color", 3.2f, 0, .1f, .4f, bLightColor},
    {"timer", 3.0f, 0, .1f, .4f, bTimer},
    {"alarm", 3.6f, 0, .05f, .3f, bAlarm},
    {"weather", 4.0f, 0, .1f, .4f, bWeather},
    {"wander", 2.4f, 0, .2f, .4f, bWander},
    {"peek", 4.6f, 0, .3f, .4f, bPeek},
    {"come closer", 4.2f, 0, .3f, .4f, bApproach},
    {"bounce", 2.4f, 0, .15f, .35f, bBounce},
    {"squish", 2.4f, 0, .2f, .3f, bSquish},
    {"sway", 4.4f, 0, .4f, .5f, bSway},
    {"battery", 3.8f, 0, .1f, .4f, bBattery},
};
// ---- the extra life of every animation, on top of what its function does: where the face goes
// (closer / up / to the side), how the head tilts, which eye is bigger or squints more, one eye a bit
// higher (looking askew), the pupils. The side is picked at random each run, so no run looks the same.
struct Flav {
  float zoom, rise, lean, tilt, big, lid, skew, pupil;  // rise px (- = up), lean px towards the side
};
static Flav flavOf(int i) {
  switch (i) {  //                       zoom   rise  lean  tilt   big    lid   skew  pupil
    case B_HAPPY: return {1.04f, -6, 0, .05f, .06f, .08f, 2, 1.15f};
    case B_LOVE: return {1.10f, -4, 0, .04f, 0, 0, 0, 1};
    case B_SURPRISED: return {1.08f, -8, 0, 0, .08f, 0, 0, .7f};
    case B_SLEEPY: return {.95f, 10, 0, .09f, .05f, .18f, 4, 1.1f};
    case B_DIZZY: return {1, 0, 0, .08f, .12f, .1f, 5, .8f};
    case B_CURIOUS: return {1.08f, -2, 12, .12f, .16f, .1f, 4, 1.3f};
    case B_WINK: return {1.03f, -2, 6, .08f, .06f, 0, 3, 1.1f};
    case B_GRUMPY: return {.94f, 4, 0, .05f, .08f, .12f, 3, .75f};
    case B_SAD: return {.94f, 12, 0, .06f, .05f, .08f, 3, 1.35f};
    case B_SUSPICIOUS: return {1, 2, 16, .07f, .12f, .18f, 4, .8f};
    case B_GIGGLE: return {1.03f, -4, 0, .06f, .06f, .06f, 2, 1.1f};
    case B_JELLY: return {1, 0, 0, .04f, .08f, 0, 0, 1};
    case B_STAR: return {1.10f, -6, 0, .03f, 0, 0, 0, 1};
    case B_CROSS: return {1, 0, 0, .05f, .15f, .05f, 4, .9f};
    case B_ROLL: return {.98f, 0, 0, .10f, .05f, .1f, 3, 1};
    case B_SHY: return {.88f, 10, 30, .12f, .06f, .06f, 4, 1.4f};
    case B_YAWN: return {1, -4, 0, .12f, .05f, .1f, 3, 1};
    case B_LOOK: return {1, 0, 0, .03f, .06f, .04f, 2, 1};
    case B_BOOP: return {.93f, 0, 0, .06f, .08f, .1f, 3, .9f};
    case B_PET: return {1.05f, 4, 0, .10f, .05f, .1f, 3, 1.2f};
    case B_WAKE: return {1, 0, 0, .05f, .06f, .15f, 3, 1};
    case B_LISTEN: return {1.06f, -2, 6, .08f, .08f, 0, 2, 1.15f};
    case B_THINK: return {1, -4, 10, .10f, .10f, .15f, 4, .9f};
    case B_HEAR: return {1.04f, 0, 8, .10f, .08f, 0, 2, 1.1f};
    case B_YES: return {1.02f, -2, 0, .03f, .04f, 0, 0, 1.1f};
    case B_NO: return {1, 0, 0, .03f, .04f, .06f, 0, .95f};
    case B_IDK: return {1, 0, 0, .12f, .12f, .1f, 5, 1};
    case B_CRY: return {.93f, 12, 0, .06f, .06f, .1f, 3, 1.4f};
    case B_SCARED: return {.85f, 6, 0, .04f, .06f, 0, 0, .6f};
    case B_PROUD: return {1.06f, -10, 0, .04f, .04f, .12f, 0, 1};
    case B_HELLO: return {1.06f, -6, 0, .08f, .06f, 0, 2, 1.2f};
    case B_FALLASLEEP: return {.94f, 10, 0, .10f, .05f, .12f, 4, 1};
    case B_SLEEP: return {.92f, 12, 0, .08f, 0, 0, 0, 1};
    case B_GLANCE: case B_DRIFT: case B_FOCUS: case B_PURSUIT: return {1, 0, 0, .04f, .05f, .04f, 2, 1};
    case B_DANCE: return {1, -4, 0, 0, .05f, 0, 0, 1.1f};
    case B_OFFLINE: return {.95f, 4, 0, .06f, .05f, .1f, 3, 1};
    case B_QUOTA: return {.94f, 10, 0, .08f, .04f, .1f, 3, 1};
    case B_CANT: case B_LOCKED: return {1, 0, 0, .05f, .06f, .06f, 2, 1};
    case B_HUH: return {1.04f, 0, 8, .14f, .14f, .06f, 4, 1};
    case B_LIGHT_ON: return {1.05f, -4, 0, .03f, .04f, 0, 0, 1};
    case B_LIGHT_OFF: return {.96f, 4, 0, .05f, .04f, .12f, 2, 1};
    case B_LIGHT_COLOR: return {1.06f, -4, 0, .05f, .05f, 0, 0, 1};
    case B_TIMER: return {1, 0, 0, .05f, .04f, 0, 2, 1};
    case B_ALARM: return {1.06f, -6, 0, 0, .06f, 0, 0, .7f};
    case B_WEATHER: return {1.04f, -2, 0, .04f, .04f, 0, 2, 1};
    case B_BATTERY: return {1.03f, 0, 0, .05f, .04f, 0, 2, 1};
    default: return {1, 0, 0, 0, 0, 0, 0, 1};  // the wandering moves steer the face themselves
  }
}
static void eyeBias(float big, float lid, float skew, float a, int bigEye);
static void flavour(int i, float w) {
  if (w <= 0.f) return;
  Flav f = flavOf(i);
  float s = (float)side;
  if (f.zoom != 1.f || f.rise != 0.f || f.lean != 0.f) roamTo(f.lean * s * w, f.rise * w, 1.f + (f.zoom - 1.f) * w);
  GA(FT, f.tilt * s * w);
  eyeBias(f.big, f.lid, f.skew * s, w, s > 0 ? 1 : 0);
  for (int k = 0; k < 2; ++k) P.e[k][PU] *= lerpf(1.f, f.pupil, w);
}
// one eye bigger, the other one squinting a little more, one sitting a little higher
static void eyeBias(float big, float lid, float skew, float a, int bigEye) {
  int small = 1 - bigEye;
  P.e[bigEye][SX] *= 1.f + big * a;
  P.e[bigEye][SY] *= 1.f + big * a;
  P.e[small][SX] *= 1.f - 0.5f * big * a;
  P.e[small][SY] *= 1.f - 0.5f * big * a;
  P.e[small][LT] += lid * a;
  P.e[0][OY] += skew * a;
  P.e[1][OY] -= skew * a;
}

// ---- resting posture: every few seconds the head tilts a little differently, one eye is a bit bigger
// or higher, one lid a bit heavier - and now and then it is symmetric again. Gentler while busy.
static float poseT = 0, pTilt = 0, pBig = 0, pSkew = 0, pLid = 0;
static void restingPose(float dt, bool calm) {
  if ((poseT -= dt * L.energy) <= 0.f) {
    poseT = frand(2.5f, 7.f);
    float liv = 0.4f + 0.6f * sat(L.energy / 1.5f);
    bool sym = frand() < 0.2f;
    pTilt = sym ? 0.f : frand(-0.09f, 0.09f) * liv * (1.f - 0.6f * L.shape);  // a tilted square looks broken
    pBig = sym ? 0.f : frand(-0.11f, 0.11f) * liv;
    pSkew = sym ? 0.f : frand(-3.5f, 3.5f) * liv;
    pLid = sym ? 0.f : frand(0.f, 0.12f) * liv;
  }
  float a = calm ? 1.f : 0.4f;
  GA(FT, pTilt * a);
  eyeBias(fabsf(pBig), pLid, pSkew, a, pBig > 0 ? 1 : 0);
}

static int cur = -1;
static int history[4] = {-1, -1, -1, -1};
int behaviorKind(int i);
static float idleTimer = 3.f;
static int demoIdx = -1;

static int rq[6], rqN = 0, rqI = 0;
static bool bOn[NB];
static float bSpeed[NB], bInt[NB];
static float lastInteract = 0;
static void startBeh(int i) {
  cur = i;
  bt = bpt = 0;
  side = (rnd() & 1) ? 1 : -1;
  errVar = (int)(rnd() % 3);
  for (int k = 3; k > 0; --k) history[k] = history[k - 1];
  history[0] = i;
}
// what it does when nobody talks to it: looking around (the table weights) plus moves and moods
// that depend on its character
static float idleWeight(int i) {
  const float e = L.energy, r = L.roam;
  switch (i) {
    case B_WANDER: return 3.f * r;
    case B_PEEK: return 0.9f * r;
    case B_APPROACH: return 0.5f * r + 0.4f * L.cheer;
    case B_BOUNCE: return 0.7f * r * e;
    case B_SQUISH: return 0.2f + 0.8f * L.cheek;
    case B_SWAY: return 0.2f + 0.7f * L.cheer;
    case B_HAPPY: return 0.2f + 1.2f * L.cheer;
    case B_GIGGLE: return 0.6f * L.cheer * (0.4f + L.cheek);
    case B_DANCE: return 0.3f * L.cheer * e;
    case B_STAR: return 0.12f * L.cheer;
    case B_CURIOUS: return 0.3f + 0.3f * L.gazeActivity;
    case B_WINK: return 0.05f + 1.f * L.cheek;
    case B_CROSS: return 0.4f * L.cheek;
    case B_JELLY: return 0.1f + 0.5f * L.cheek;
    case B_YAWN: return 0.05f + 1.4f * L.sleepy;
    case B_SLEEPY: return 0.5f * L.sleepy;
    case B_SHY: return 0.05f + 1.1f * L.shy;
    case B_GRUMPY: return 1.f * L.grumpy;
    case B_ROLL: return 0.05f + 0.9f * L.grumpy;
    case B_SUSPICIOUS: return 0.1f + 0.7f * L.grumpy;
  }
  return BEH[i].weight;
}
static void pickBeh() {
  float total = 0;
  float wts[NB];
  for (int i = 0; i < NB; ++i) {
    float wv = bOn[i] ? idleWeight(i) : 0.f;
    if (history[0] == i) wv = 0;  // never the same twice in a row
    wts[i] = wv;
    total += wv;
  }
  float r = frand() * total;
  for (int i = 0; i < NB; ++i) {
    r -= wts[i];
    if (r <= 0 && wts[i] > 0) {
      startBeh(i);
      return;
    }
  }
  startBeh(B_HAPPY);
}

// ============================================================== idle gaze
static float gzTx = 0, gzTy = 0, gzTimer = 1.f, microTimer = 0, microX = 0, microY = 0;
static int burst = 0;
static void idleGaze(float dt, bool paused) {
  if (!paused) {
    gzTimer -= dt;
    if (gzTimer <= 0) {
      float nx, ny;
      if (frand() < 0.3f) {
        nx = frand(-0.2f, 0.2f);
        ny = frand(-0.15f, 0.2f);
      } else {
        nx = frand(-1.f, 1.f);
        ny = frand(-0.7f, 0.7f);
      }
      float dist = sqrtf((nx - gzTx) * (nx - gzTx) + (ny - gzTy) * (ny - gzTy));
      gzTx = nx;
      gzTy = ny;
      if (burst > 0) {
        burst--;
        gzTimer = frand(0.18f, 0.35f);
      } else if (frand() < 0.14f) {
        burst = 2 + rnd() % 3;
        gzTimer = frand(0.2f, 0.35f);
      } else {
        float r = frand();
        gzTimer = (0.5f + 3.f * r * r) / (maxf(0.2f, L.gazeActivity) * sqrtf(clampf(L.energy, 0.3f, 2.f)));
      }
      if (dist > 0.75f && frand() < 0.35f && !blinkSuppress) startBlink(1.15f);
    }
  }
  microTimer -= dt;
  if (microTimer <= 0) {
    microTimer = frand(0.15f, 0.6f);
    microX = frand(-0.03f, 0.03f);
    microY = frand(-0.025f, 0.025f);
  }
  P.g[GX] = gzTx + microX;
  P.g[GY] = gzTy + microY;
}

// ============================================================== touch
static bool tDown = false, tPrev = false, tMoved = false;
static float tX = 0, tY = 0, tSX = 0, tSY = 0, tTime = 0, tapWindow = 0, touchHold = 0;
static int tapCount = 0;

void touch(bool down, int x, int y) {
  tDown = down;
  if (down) {
    tX = (float)x;
    tY = (float)y;
  }
}
static const int DEMO[] = {B_HELLO, B_YES, B_NO, B_IDK, B_HAPPY, B_GIGGLE, B_LOVE, B_STAR, B_PROUD, B_SAD,
                           B_CRY, B_SURPRISED, B_SCARED, B_GRUMPY, B_CURIOUS, B_SHY, B_WINK, B_DIZZY, B_ROLL,
                           B_SUSPICIOUS, B_DANCE, B_OFFLINE, B_QUOTA, B_CANT, B_HUH, B_LOCKED, B_GLITCH,
                           B_LIGHT_ON, B_LIGHT_COLOR, B_LIGHT_OFF, B_TIMER, B_ALARM, B_WEATHER,
                           B_WANDER, B_PEEK, B_APPROACH, B_BOUNCE, B_SQUISH, B_SWAY, B_FALLASLEEP};
void buttonClick() {
  demoIdx = (demoIdx + 1) % (int)(sizeof(DEMO) / sizeof(DEMO[0]));
  rqN = 0;
  lastInteract = T;
  startBeh(DEMO[demoIdx]);
}

// names the voice model may answer with
static const struct {
  const char* name;
  int beh;
} REACT[] = {
    {"yes", B_YES},          {"no", B_NO},           {"dont_know", B_IDK},       {"happy", B_HAPPY},
    {"laugh", B_GIGGLE},     {"love", B_LOVE},       {"excited", B_STAR},        {"proud", B_PROUD},
    {"sad", B_SAD},          {"cry", B_CRY},         {"surprised", B_SURPRISED}, {"scared", B_SCARED},
    {"angry", B_GRUMPY},     {"confused", B_CURIOUS}, {"thinking", B_THINK},     {"shy", B_SHY},
    {"wink", B_WINK},        {"hello", B_HELLO},     {"sleep", B_FALLASLEEP},    {"dizzy", B_DIZZY},
    {"bored", B_ROLL},       {"suspicious", B_SUSPICIOUS}, {"curious", B_CURIOUS}, {"dance", B_DANCE},
    {"wander", B_WANDER},    {"peek", B_PEEK},       {"come_closer", B_APPROACH}, {"bounce", B_BOUNCE},
    {"squish", B_SQUISH},    {"sway", B_SWAY},       {"battery", B_BATTERY},
};
void react(const char* const* names, int n) {
  thinkWait = false;
  rqN = rqI = 0;
  for (int i = 0; i < n && rqN < 5; ++i)
    for (auto& r : REACT)
      if (!strcmp(r.name, names[i])) {
        if (!bOn[r.beh]) break;  // switched off in the settings
        rq[rqN++] = r.beh;
        if (r.beh == B_FALLASLEEP) rq[rqN++] = B_SLEEP;
        break;
      }
  lastInteract = T;
  if (rqN == 0) {  // nothing (enabled) to show
    if (cur == B_THINK || cur == B_LISTEN) cur = -1;
    return;
  }
  startBeh(rq[0]);
}
static float lookT = 0, lookGX = 0, lookGY = 0;
void lookAt(float gx, float gy, float secs) {
  lookGX = clampf(gx, -1.f, 1.f);
  lookGY = clampf(gy, -1.f, 1.f);
  lookT = secs;
  lastInteract = T;
}
void listenDone(bool gotSpeech) {
  if (cur != B_LISTEN) return;
  if (gotSpeech) {
    thinkWait = true;
    rqN = 0;
    startBeh(B_THINK);
  } else {
    const char* hm = "curious";
    react(&hm, 1);
  }
}
void error(const char* name) {
  int i = -1;
  for (int k = B_OFFLINE; k <= B_GLITCH; ++k)
    if (!strcmp(BEH[k].name, name)) i = k;
  thinkWait = false;
  lastInteract = T;
  if (i < 0 || !bOn[i]) {
    if (cur == B_THINK || cur == B_LISTEN) cur = -1;
    return;
  }
  rqN = 0;
  startBeh(i);
}
static void startHome(int i) {
  if (!bOn[i]) return;
  thinkWait = false;
  lastInteract = T;
  rqN = 0;
  startBeh(i);
}
void home(const char* name, uint32_t rgb) {
  int i = behaviorIndex(name);
  if (i < B_LIGHT_ON || !bOn[i]) return;
  bulbFrom = glyphCol;
  bulbCol = {(float)(rgb >> 16 & 255), (float)(rgb >> 8 & 255), (float)(rgb & 255)};
  startHome(i);
}
void timer(bool ringing) { startHome(ringing ? B_ALARM : B_TIMER); }
void music(float amount, float bpm, float beatPos, float loud, float drive, bool mayWake) {
  musicLoud = clampf(loud, 0.f, 1.f);
  musicDrive = clampf(drive, 0.f, 1.f);
  if (musicIn <= 0.01f && amount > 0.f) musicPos = beatPos;  // starts in time
  if (mayWake && amount > 0.6f && L.music && cur == B_SLEEP) {  // dozed off from boredom: music wakes it up
    lastInteract = T;
    rqN = 0;
    startBeh(B_WAKE);
  }
  musicIn = clampf(amount, 0.f, 1.f);
  musicBpm = clampf(bpm, 30.f, 190.f);
  musicTarget = beatPos;
}
void sequence(const int* beh, int n) {
  thinkWait = false;
  rqN = rqI = 0;
  for (int i = 0; i < n && rqN < 6; ++i)
    if (beh[i] >= 0 && beh[i] < NB && bOn[beh[i]] && behaviorKind(beh[i]) != K_INTERNAL) rq[rqN++] = beh[i];
  lastInteract = T;
  if (rqN) startBeh(rq[0]);
}
void battery(int level, bool charging, bool show) {
  battLevel = level;
  battCharging = charging;
  if (show) startHome(B_BATTERY);
}
void weather(const char* cond) {
  static const char* N[] = {"sunny", "partlycloudy", "cloudy", "rainy", "snowy", "lightning", "fog", "clear-night"};
  weatherCond = W_CLOUD;
  for (int k = 0; k < 8; ++k)
    if (!strcmp(cond, N[k])) weatherCond = k;
  // Home Assistant has a few more: map them onto the eight symbols
  if (!strcmp(cond, "pouring")) weatherCond = W_RAIN;
  if (!strcmp(cond, "snowy-rainy") || !strcmp(cond, "hail")) weatherCond = W_SNOW;
  if (!strcmp(cond, "lightning-rainy")) weatherCond = W_STORM;
  if (!strcmp(cond, "windy") || !strcmp(cond, "windy-variant")) weatherCond = W_CLOUD;
  startHome(B_WEATHER);
}
static RGB moodCol = {0, 0, 0};
static float moodTarget = 0, moodAmt = 0;
void moodLight(bool on, uint32_t rgb) {
  moodTarget = on ? 1.f : 0.f;
  if (on) moodCol = {(float)(rgb >> 16 & 255), (float)(rgb >> 8 & 255), (float)(rgb & 255)};
}
void listenMood(const char* name) {
  static const char* N[] = {"none", "happy", "sad", "surprised", "love", "curious", "funny", "worried"};
  for (int k = 0; k < 8; ++k)
    if (!strcmp(N[k], name)) {
      if (k != MOOD_NONE) {
        moodK = k;
        moodSetT = T;
      }
      return;
    }
}
void busy(bool on) {
  if (on) {
    if (cur >= 0 && behaviorKind(cur) == K_REACTION) return;  // let a reaction finish
    thinkWait = true;
    rqN = 0;
    if (cur != B_THINK) startBeh(B_THINK);
  } else if (thinkWait) {
    thinkWait = false;
    if (cur == B_THINK) cur = -1;
  }
}
bool reacting() {
  if (cur < 0) return false;
  int k = behaviorKind(cur);
  return (k == K_REACTION || k == K_ERROR || k == K_HOME) && bt < BEH[cur].dur - BEH[cur].release;
}
void buttonLong() { startBeh(B_COLOR); }
void wakeWord() { wakeReq = true; }
static float spkIn = -1.f, spk = 0, spkSlow = 0, spkT = 0;
void speaking(float level) { spkIn = level; }
bool asleep() { return cur == B_SLEEP || cur == B_FALLASLEEP; }
void voice(bool active, float level, float pitch) {
  vActive = active;
  vLevelIn = clampf(level, 0.f, 1.f);
  vPitchIn = clampf(pitch, 0.f, 1.f);
}

static void handleTouch(float dt) {
  if (tDown) {
    lastInteract = T;
    if (!tPrev && (cur == B_SLEEP || cur == B_FALLASLEEP)) {
      rqN = 0;
      startBeh(B_WAKE);
    }
    if (!tPrev) {
      tSX = tX;
      tSY = tY;
      tTime = 0;
      tMoved = false;
    }
    tTime += dt;
    if (fabsf(tX - tSX) + fabsf(tY - tSY) > 22) tMoved = true;
    touchGX = clampf((tX - CX) / 80.f, -1.f, 1.f);
    touchGY = clampf((tY - CY) / 80.f, -1.f, 1.f);
    if (tTime > 0.7f && !tMoved && cur != B_PET && cur != B_WAKE && bOn[B_PET]) startBeh(B_PET);
    if (cur != B_PET) touchHold = 1.f;
  } else if (tPrev) {
    if (cur == B_PET) {
      if (bOn[B_LOVE]) startBeh(B_LOVE);
      else cur = -1;
    } else if (tTime < 0.45f && !tMoved && cur != B_WAKE) {
      tapCount++;
      tapWindow = 2.2f;
      if (tapCount >= 4) {
        int b = (rnd() & 1) ? B_GRUMPY : B_DIZZY;
        if (bOn[b]) startBeh(b);
        tapCount = 0;
      } else if (bOn[B_BOOP]) {
        startBeh(B_BOOP);
      }
    }
    idleTimer = frand(4.f, 7.f);
  }
  tPrev = tDown;
  tapWindow -= dt;
  if (tapWindow <= 0) tapCount = 0;
  if (!tDown) touchHold = maxf(0.f, touchHold - dt * 2.f);
}

// ============================================================== compose
static inline float noise3(float t, float s) {
  return 0.5f * fsin(1.3f * t + s) + 0.3f * fsin(2.9f * t + 2.1f * s) + 0.2f * fsin(4.7f * t + 0.7f * s);
}
static inline Rect unite(const Rect& a, const Rect& b) {
  if (a.empty()) return b;
  if (b.empty()) return a;
  return {a.x0 < b.x0 ? a.x0 : b.x0, a.y0 < b.y0 ? a.y0 : b.y0, a.x1 > b.x1 ? a.x1 : b.x1,
          a.y1 > b.y1 ? a.y1 : b.y1};
}
static inline Rect clipR(Rect r) {
  if (r.x0 < 0) r.x0 = 0;
  if (r.y0 < 0) r.y0 = 0;
  if (r.x1 > W) r.x1 = W;
  if (r.y1 > H) r.y1 = H;
  return r;
}
static inline Rect boxAround(float x, float y, float ex, float ey) {
  return clipR({(int)floorf(x - ex), (int)floorf(y - ey), (int)ceilf(x + ex) + 1, (int)ceilf(y + ey) + 1});
}

static void composeEye(int i, float dt, float fx, float fy, float ft, float stretch, float hstretch, float lean, float hx,
                       float breath, float wob, float zoom) {
  EyeR& E_ = F.eye[i];
  float e[NE];
  for (int c = 0; c < NE; ++c) e[c] = SE[i][c].x;
  float sg = i == 0 ? -1.f : 1.f;
  float inner = -sg;
  float b = blinkVal[i];

  float turn = sg * hx;
  float psx = 1.f - 0.13f * maxf(turn, 0.f) + 0.05f * maxf(-turn, 0.f);
  float psy = 1.f - 0.035f * maxf(turn, 0.f) + 0.03f * maxf(-turn, 0.f);
  float sep = SEP * zoom * (1.f - 0.07f * fabsf(hx));
  float as = zoom * (1.f + (i == 0 ? L.asym : -0.5f * L.asym));  // one eye a little bigger
  float n1 = noise3(T * 0.8f, 1.7f + i * 4.1f), n2 = noise3(T * 0.7f, 3.3f + i * 2.3f),
        n3 = noise3(T * 0.6f, 5.1f + i * 1.9f);

  float sx = as * e[SX] * psx * breath * (1.f - stretch * 0.6f + hstretch) * (1.f + 0.05f * b) * (1.f + 0.015f * n1);
  float sy = as * e[SY] * psy * breath * (1.f + stretch - hstretch * 0.6f) * (1.f - 0.07f * b) * (1.f + 0.015f * n2);
  float cr = fcos(ft), sr = fsin(ft);
  float lx0 = sg * sep + e[OX], ly0 = e[OY] + 2.5f * b;
  E_.cx = CX + fx + lx0 * cr - ly0 * sr;
  E_.cy = CY + fy + lx0 * sr + ly0 * cr;
  float tilt = sg * BASE_TILT * (1.f - 0.6f * L.shape) + e[TILT] + ft + lean + 0.025f * n3;
  E_.c = cosf(tilt);
  E_.s = sinf(tilt);
  E_.rx = RX * sx;
  E_.ryT = RYT * sy;
  E_.ryB = RYB * sy;
  E_.egg = -0.04f;
  E_.irx2 = 1.f / (E_.rx * E_.rx);
  E_.sirx = 1.f / E_.rx;
  E_.siryT = 1.f / E_.ryT;
  E_.siryB = 1.f / E_.ryB;

  // ---- lids
  float th = STROKE * (0.8f * zoom + 0.2f * sy);
  float lt = clampf(e[LT], 0.f, 1.f), lb = clampf(e[LB], 0.f, 1.f);
  lt = lt + (1.f - lt) * b;
  float yT = lerpf(-1.3f * E_.ryT, E_.ryB - th, lt);
  float yB = lerpf(1.3f * E_.ryB, -E_.ryT + th, lb);
  if (yB - yT < th) {
    float m = 0.5f * (yB + yT);
    yT = m - 0.5f * th;
    yB = m + 0.5f * th;
  }
  E_.yT = yT;
  E_.yB = yB;
  E_.kT = e[ST] * inner * 0.45f;
  E_.kB = e[SB] * inner * 0.45f;
  E_.lidT = lt > 0.001f || fabsf(E_.kT) > 0.001f;
  E_.lidB = lb > 0.001f || fabsf(E_.kB) > 0.001f;
  E_.sqK = shapeK(L.shape);
  E_.sqR = shapeR(L.shape, 0.44f);
  E_.isqR = shapeR(L.shape, 0.33f);
  E_.lcT = 1.f - 0.8f * E_.sqK;
  // the resting face's straight lids (as drawn in the web interface): heavy lids, a smile line, a stern or
  // worried slant. They draw back when the eyes open wide (surprised, scared).
  {
    float H = E_.ryT + E_.ryB;
    // they also give way to an animation's own lids (suspicious, sleepy, happy): its expression wins
    float open = sat(1.f - 3.f * maxf(0.f, e[SY] - 1.f));
    float ot = open * sat(1.f - 1.6f * e[LT]), ob = open * sat(1.f - 1.6f * e[LB]);
    float ft = L.lidTop * ot, fb = L.lidBottom * ob, br = L.brow * ot;
    E_.flT = ft > 0.001f || fabsf(br) > 0.001f;
    E_.flB = fb > 0.001f;
    E_.fT = -E_.ryT + ft * H;
    E_.fTk = br * 0.21f * H * inner / E_.rx;
    E_.fTn = 1.f / sqrtf(1.f + E_.fTk * E_.fTk);
    E_.fB = E_.ryB - fb * H;
  }

  auto sAt = [&](float x) {
    float u = x / E_.rx;
    return sqrtf(maxf(0.f, 1.f - u * u));
  };
  auto topAt = [&](float x) {
    float t = maxf(-E_.ryT * sAt(x), yT * (1.f + (sAt(x) - 1.f) * E_.lcT) + E_.kT * x);
    return E_.flT ? maxf(t, E_.fT + E_.fTk * x) : t;
  };
  auto botAt = [&](float x) {
    float b = minf(E_.ryB * sAt(x), yB * sAt(x) + E_.kB * x);
    return E_.flB ? minf(b, E_.fB) : b;
  };
  float X = 0.7f * E_.rx;
  float m0 = 0.5f * (topAt(0) + botAt(0)), mL = 0.5f * (topAt(-X) + botAt(-X)), mR = 0.5f * (topAt(X) + botAt(X));
  float gap = botAt(0) - topAt(0);
  E_.closeK = 1.f - sstep(th * 1.05f, th * 2.4f, gap);
  E_.c0 = m0;
  E_.c1 = (mR - mL) / (2.f * X);
  E_.c2 = (mR + mL - 2.f * m0) / (2.f * X * X);
  E_.L = 0.8f * E_.rx;
  E_.hT = 0.5f * th;

  // ---- chevrons  > <
  E_.chev = sat(e[CHEV]);
  E_.chevDir = i == 0 ? 1.f : -1.f;
  E_.chW = 0.42f * E_.rx;
  E_.chH = 0.5f * E_.ryT;
  E_.chT = 0.5f * th * 1.1f;
  E_.chBx = 1.9f * E_.chW;
  E_.chBy = E_.chH;
  E_.chIbb = 1.f / (E_.chBx * E_.chBx + E_.chBy * E_.chBy);

  // ---- iris
  float gx = clampf(SG[GX].x + e[GXO] + inner * 0.08f, -1.25f, 1.25f);
  float gy = clampf(SG[GY].x + e[GYO] + 0.12f, -1.2f, 1.2f);
  float ilx = gx * E_.rx * 0.62f;
  float ily = gy * (gy < 0 ? E_.ryT : E_.ryB) * 0.5f + 6.f * b;
  E_.ix = E_.cx + ilx * E_.c - ily * E_.s;
  E_.iy = E_.cy + ilx * E_.s + ily * E_.c;
  float is = e[IS], ia = maxf(0.3f, e[IA]);
  float fsx = 1.f - 0.2f * gx * gx, fsy = 1.f - 0.14f * gy * gy;
  float sia = sqrtf(ia);
  E_.irx = RX * IRX * is / sia * fsx * sx;
  E_.iryT = RYT * IRYT * is * sia * fsy * sy;
  E_.iryB = RYB * IRYB * is * sia * fsy * sy;
  E_.iegg = IEGG;
  E_.iirx = 1.f / E_.irx;
  E_.iiryT = 1.f / E_.iryT;
  E_.iiryB = 1.f / E_.iryB;
  // motion stretch along the gaze velocity (in eye-local pixels)
  float vpx = (SG[GX].v + SE[i][GXO].v) * E_.rx * 0.62f;
  float vpy = (SG[GY].v + SE[i][GYO].v) * E_.ryB * 0.5f;
  float sp = sqrtf(vpx * vpx + vpy * vpy);
  float st = 1.f + clampf(sp * 0.0011f, 0.f, 0.3f);
  float dcx = 1.f, dcy = 0.f;
  if (sp > 1e-3f) {
    dcx = vpx / sp;
    dcy = vpy / sp;
  }
  float ist = 1.f / st;
  float a00 = dcx * dcx * ist + dcy * dcy * st, a01 = dcx * dcy * (ist - st), a11 = dcy * dcy * ist + dcx * dcx * st;
  // M = S^-1 * R(-tilt)
  E_.m00 = a00 * E_.c - a01 * E_.s;
  E_.m01 = a00 * E_.s + a01 * E_.c;
  E_.m10 = a01 * E_.c - a11 * E_.s;
  E_.m11 = a01 * E_.s + a11 * E_.c;
  float imax = maxf(E_.irx, maxf(E_.iryT, E_.iryB));
  // the springs only approach 0: a tiny rest would keep the expensive paths (whole eye) on for good
  auto cut = [](float v) { return v < 0.01f ? 0.f : v; };
  E_.heart = cut(sat(e[HEART]));
  E_.hs = 0.85f * E_.rx * is;
  E_.ihs = 1.f / E_.hs;
  E_.star = cut(sat(e[STAR]));
  E_.sr = 0.6f * E_.rx * is;
  E_.starC = fcos(starRot * sg);
  E_.starS = fsin(starRot * sg);
  E_.glyph = cut(sat(e[GLY]));
  E_.gs = 0.64f * E_.rx * clampf(is, 0.7f, 1.3f);
  E_.igs = 1.f / E_.gs;
  E_.grc = fcos(glyphRot * sg);
  E_.grs = fsin(glyphRot * sg);
  // the symbol sits between the eye centre and the iris: it follows the gaze but stays inside
  E_.gcx = 0.5f * (E_.cx + E_.ix);
  E_.gcy = 0.5f * (E_.cy + E_.iy);
  float shapeR = maxf(imax, maxf(E_.heart > 0 ? E_.hs * 0.75f : 0.f, E_.star > 0 ? E_.sr : 0.f));
  E_.ibR = shapeR * st * 1.1f + 3.f;
  E_.pr = L.pupil > 0.01f ? minf(E_.irx, E_.iryT) * 0.6f * L.pupil * e[PU] * (1.f - E_.heart) * (1.f - E_.star) : 0.f;
  E_.irisA = (1.f - E_.closeK) * sat(1.f - 2.2f * E_.chev);

  // ---- highlights
  E_.hl = sat(e[HL]) * (1.f - E_.glyph);
  {
    float hx1 = -0.34f * E_.irx, hy1 = -0.40f * E_.iryT;
    float hx2 = 0.30f * E_.irx, hy2 = 0.36f * E_.iryB;
    E_.h1x = E_.ix + hx1 * E_.c - hy1 * E_.s;
    E_.h1y = E_.iy + hx1 * E_.s + hy1 * E_.c;
    E_.h2x = E_.ix + hx2 * E_.c - hy2 * E_.s;
    E_.h2y = E_.iy + hx2 * E_.s + hy2 * E_.c;
    E_.h1r = 0.36f * E_.irx * E_.hl;
    E_.h2r = 0.17f * E_.irx * E_.hl;
  }

  E_.wobA = wob;
  E_.wobPh = T * 13.f + i * 1.3f;
  // half block diagonal (2.12px) * Lipschitz bound + AA half width
  float lw = 1.f + 0.071f * wob;
  E_.Ms = 2.13f * 1.12f * lw + 0.55f;
  E_.Mi = 2.13f * 1.12f * st * lw + 0.55f;

  float rxm = E_.rx * (1.f + fabsf(E_.egg)), rym = maxf(E_.ryT, E_.ryB);
  float ex = sqrtf(rxm * rxm * E_.c * E_.c + rym * rym * E_.s * E_.s) + wob + 2.f;
  float ey = sqrtf(rxm * rxm * E_.s * E_.s + rym * rym * E_.c * E_.c) + wob + 2.f;
  if (E_.sqK > 0.f) {  // a square reaches further into the corners when tilted
    ex = rxm * fabsf(E_.c) + rym * fabsf(E_.s) + wob + 2.f;
    ey = rxm * fabsf(E_.s) + rym * fabsf(E_.c) + wob + 2.f;
  }
  E_.bb = boxAround(E_.cx, E_.cy, ex + glitchAmt, ey);
}

static void composeFrame(float dt) {
  for (int i = 0; i < 2; ++i)
    for (int c = 0; c < NE; ++c) SE[i][c].step(P.e[i][c], dt);
  for (int c = 0; c < NG; ++c) SG[c].step(P.g[c], dt);

  wobA *= expf(-2.6f * dt);
  float wob = L.wobble ? maxf(wobA, P.wob) : 0.f;

  for (int k = 0; k < 3; ++k) SR[k].step(k == 0 ? roamTX : k == 1 ? roamTY : roamTZ, dt);
  float zoom = clampf(SR[2].x, 0.5f, 1.5f);
  // always a little restless drift when it likes to wander
  float drift = L.roam * (0.4f + 0.6f * L.energy);
  float fx = SG[FX].x + P.dx + SR[0].x + 6.f * drift * noise3(T * 0.31f, 9.1f);
  float fy = SG[FY].x + P.dy + SR[1].x + 4.f * drift * noise3(T * 0.27f, 4.4f), ft = SG[FT].x + P.drot;
  float vy = SG[FY].v + SR[1].v + (dt > 0 ? (P.dy - prevDy) / dt : 0.f);
  prevDy = P.dy;
  vyS = lerpf(vyS, vy, 0.45f);
  float stretch = clampf(-vyS * 0.0018f, -0.1f, 0.12f) * L.squash;
  vxS = lerpf(vxS, SG[FX].v + SR[0].v + (dt > 0 ? (P.dx - prevDx) / dt : 0.f), 0.45f);
  prevDx = P.dx;
  float lean = clampf(-vxS * 0.0016f, -0.09f, 0.09f);
  float hstretch = clampf(fabsf(vxS) * 0.0012f, 0.f, 0.08f) * L.squash;
  float hx = clampf(SG[FX].x / 12.f, -1.f, 1.f);
  float breath = 1.f + 0.012f * L.breath * fsin(TAU * T / 4.3f);
  fy += 1.2f * L.breath * fsin(TAU * T / 4.3f - 0.6f);

  for (int i = 0; i < 2; ++i) composeEye(i, dt, fx, fy, ft, stretch, hstretch, lean, hx, breath, wob, zoom);

  // colours
  cwT = minf(1.f, cwT + dt / 0.12f);
  float ct = sstep(0, 1, cwT);
  const Colorway &A = cwAt(cwPrev), &B = cwAt(cwCur);
  F.scl = mix(A.sclera, B.sclera, ct);
  F.iris = mix(A.iris, B.iris, ct);
  F.acc = mix(A.accent, B.accent, ct);
  F.hl = mix(A.hl, B.hl, ct);
  F.fx = mix(A.fx, B.fx, ct);
  F.bg = bgCol;
  F.gcol = glyphCol;
  F.pup = mix(F.iris, RGB{0, 0, 0}, 0.8f);
  moodAmt += (moodTarget - moodAmt) * minf(1.f, dt * 2.f);  // the lamp colour fades in and out
  if (moodAmt > 0.002f) F.iris = mix(F.iris, moodCol, moodAmt);
  F.glyph = glyphType;
  for (int k = 0; k < 4; ++k) F.gp[k] = gp[k];
  F.tr[0] = fsin(gp[0]);
  F.tr[1] = fcos(gp[0]);
  F.tr[2] = fsin(gp[1]);
  F.tr[3] = fcos(gp[1]);
  for (int k = 0; k < 8; ++k) {
    float a = gp[0] * 0.6f + k * 0.785398f;
    F.tr[4 + 2 * k] = fsin(a);
    F.tr[5 + 2 * k] = fcos(a);
  }
  F.glitch = glitchAmt > 0.3f ? glitchAmt : 0.f;
  F.glitchSeed = glitchSeed;

  // blush
  float ba = L.blush ? sat(SG[BLUSH].x) : 0.f;
  for (int i = 0; i < 2; ++i) {
    BlushR& bl = F.blush[i];
    const EyeR& e = F.eye[i];
    float sg = i == 0 ? -1.f : 1.f;
    bl.cx = e.cx + sg * 10.f * zoom;
    bl.cy = e.cy + e.ryB * 0.95f + 9.f * zoom;
    bl.rx = 17.f * zoom;
    bl.ry = 7.5f * zoom;
    bl.a = ba;
    bl.bb = ba > 0.01f ? boxAround(bl.cx, bl.cy, bl.rx + 3, bl.ry + 3) : Rect{0, 0, 0, 0};
  }

  // sprites
  F.nspr = 0;
  for (auto& s : spr) {
    if (!s.alive) continue;
    SprR& r = F.spr[F.nspr++];
    r.type = s.type;
    float pin = s.age < 0.3f ? sat(s.age / 0.3f) : 1.f;
    const float c1 = 2.2f;
    float u = pin - 1.f;
    float popIn = 1.f + (c1 + 1.f) * u * u * u + c1 * u * u;  // ease out back
    float out = sat((s.life - s.age) / 0.35f);
    r.sc = s.size * popIn * (0.4f + 0.6f * out);
    r.a = out;
    r.x = s.x + s.wig * fsin(s.age * TAU / s.wigF + s.ph);
    r.y = s.y;
    float rot = s.rot + (s.wig > 0 ? 0.15f * fsin(s.age * TAU / s.wigF + s.ph + 1.f) : 0.f);
    r.c = fcos(rot);
    r.s = fsin(rot);
    r.col = s.col == 0 ? F.acc : s.col == 1 ? F.fx : s.col == 2 ? RGB{255, 255, 255} : s.col == 3 ? TEAR_COL : ANGER_COL;
    float ext = r.sc * 1.25f + 2.f;
    r.bb = boxAround(r.x, r.y, ext, ext);
    if (r.bb.empty()) F.nspr--;
  }

  Rect box = unite(F.eye[0].bb, F.eye[1].bb);
  for (int i = 0; i < 2; ++i) box = unite(box, F.blush[i].bb);
  for (int i = 0; i < F.nspr; ++i) box = unite(box, F.spr[i].bb);
  F.dirty = clipR(unite(box, prevBox));
  prevBox = box;
}

// ============================================================== public API
void init(uint32_t seed) {
  for (int i = 0; i < NB; ++i) {
    bOn[i] = true;
    bSpeed[i] = 1.f;
    bInt[i] = 1.f;
  }
  rs = seed ? seed : 0x9e3779b9u;
  for (int i = 0; i < 8; ++i) rnd();
  STAR5 = starK(5, 2.8f);
  STAR4 = starK(4, 3.2f);
  for (int i = 0; i < 2; ++i)
    for (int c = 0; c < NE; ++c) {
      SE[i][c].tune(EF[c], EZ[c]);
      SE[i][c].x = ENEU[c];
      SE[i][c].v = 0;
    }
  for (int c = 0; c < NG; ++c) {
    SG[c].tune(GF[c], GZ[c]);
    SG[c].x = SG[c].v = 0;
  }
  SR[0].tune(0.85f, 0.62f);  // a little overshoot when it arrives somewhere
  SR[1].tune(0.85f, 0.62f);
  SR[2].tune(0.7f, 0.75f);
  SR[0].x = SR[1].x = 0;
  SR[2].x = 1.f;
  // wake up: start with closed eyes that open
  SE[0][LT].x = SE[1][LT].x = 1.f;
  memset(spr, 0, sizeof(spr));
  cwCur = cwPrev = (CFG_COLORWAY >= 0 && CFG_COLORWAY < NCW) ? CFG_COLORWAY : 0;
  cwT = 1;
  idleTimer = 4.f;
  startBeh(B_WAKE);
  composeFrame(0.f);
}

// ---- dancing to music, matched to it:
//   calm (slow and medium beats): the head bobs - down on the beat, easing back up - with relaxed lids,
//     a slow sway over two beats, the gaze drifting along; now and then it closes its eyes and enjoys it
//   in between: side to side on every beat
//   driving (fast, busy music): little hops with squash and stretch, steps to the side, eyes squishing in
//     turn, a wobble on the one
// A new figure every 8 beats, the first beat of a bar is a little stronger, louder music makes it bigger
// and brings the face a little closer. The styles blend smoothly as the music changes.
static float mDrive = 0, mLoud = 0, stepX = 0;
static int figBlock = -1, fig = 0, figSide = 1, lastBeat = -1;
static void groove(float dt) {
  mDrive = lerpf(mDrive, musicDrive, 1.f - expf(-dt * 0.5f));  // the style changes over ~2 s
  mLoud = lerpf(mLoud, musicLoud, 1.f - expf(-dt * 1.5f));
  const float s = musicS, sq = clampf(L.squash, 0.3f, 1.6f);
  const int beat = (int)ffloor(musicPos);
  const float ph = musicPos - beat;
  const int inBar = beat & 3;
  if ((beat >> 3) != figBlock) {  // a new figure every 8 beats
    figBlock = beat >> 3;
    fig = (int)(rnd() % 3);
    figSide = (rnd() & 1) ? 1 : -1;
  }
  const bool onBeat = beat != lastBeat;
  lastBeat = beat;
  const float A = s * (0.5f + 0.8f * mLoud) * (inBar == 0 ? 1.25f : 1.f);
  const float calm = 1.f - sstep(0.35f, 0.7f, mDrive), drv = sstep(0.5f, 0.85f, mDrive);
  const float mid = maxf(0.f, 1.f - calm - drv);
  const float nod = 0.5f + 0.5f * fcos(TAU * ph), nod2 = nod * nod;  // 1 on the beat: head down
  const float down = expf(-ph * 6.f);                                  // the impact, fading
  const float air = 4.f * ph * (1.f - ph);                             // a hop between two beats
  const float sway2 = fsin(PI * musicPos);                             // over two beats
  const float sw = (beat & 1) ? 1.f : -1.f;

  // all styles: pupils pulse with the beat, a slight squash as it lands
  for (int k = 0; k < 2; ++k) {
    P.e[k][PU] *= 1.f + 0.2f * down * A;
    P.e[k][SY] *= 1.f - (0.05f * calm + 0.08f * mid + 0.14f * drv) * down * A * sq;
    P.e[k][SX] *= 1.f + (0.035f * calm + 0.06f * mid + 0.1f * drv) * down * A * sq;
  }

  // calm: head bob
  if (calm > 0.f) {
    float c = A * calm;
    P.dy += 15.f * c * nod2;
    G(GY, -0.15f + 0.3f * nod2, s * calm * 0.6f);  // the eyes nod along: down on the beat, up between
    GA(FT, 0.07f * sway2 * c * (fig == 1 ? 1.6f : 1.f));
    if (fig == 0) P.dx += 7.f * sway2 * c;
    if (fig == 2 && (beat & 7) < 2) {  // a happy squint, enjoying it (^ ^)
      float k = s * calm * sstep(0.f, 0.25f, ph + (beat & 1));
      E(LB, 0.8f, k);
      E(LT, 0.2f, k);
    } else {
      E(LB, 0.22f, s * calm);
      E(LT, 0.15f, s * calm * 0.8f);
    }
    G(GX, 0.3f * sway2 * figSide, s * calm * 0.5f);
    if (onBeat && inBar == 0 && fig == 0 && (rnd() & 3) == 0) startBlink(1.2f);
  }
  // in between: side to side on every beat
  if (mid > 0.f) {
    float m = A * mid;
    P.dy += 12.f * m * down;
    P.dx += 16.f * sw * fsin(PI * ph) * m;
    GA(FT, 0.11f * sw * fsin(PI * ph) * m);
    G(GX, 0.5f * sw, s * mid * 0.6f);
    E(LB, 0.25f, s * mid);
    if (fig == 1) GA(FT, 0.05f * fsin(TAU * musicPos / 4.f) * m);  // a slow roll over the bar on top
  }
  // driving: hops, steps, eye squishes
  if (drv > 0.f) {
    float d = A * drv;
    P.dy -= 18.f * air * d;
    for (int k = 0; k < 2; ++k) P.e[k][SY] *= 1.f + 0.06f * air * d * sq;  // stretched in the air
    E(LB, 0.35f, s * drv);
    G(BLUSH, 0.6f, s * drv);
    if (fig == 0) GA(FT, 0.09f * sw * d);
    if (fig == 1) {  // steps: every two beats to the other side, the eyes lead
      if (onBeat && !(beat & 1)) stepX = ((beat >> 1) & 1 ? 1.f : -1.f) * figSide * 26.f;
      G(GX, stepX / 40.f, s * drv * 0.7f);
    }
    if (fig == 2) {  // the eyes squish in turn
      int k = beat & 1;
      P.e[k][LB] += 0.6f * down * d;
      P.e[k][SY] *= 1.f - 0.1f * down * d;
      GA(FT, 0.05f * sw * d);
    }
    if (onBeat && inBar == 0) {
      wobKick(1.4f * d);
      if (!(beat & 7) && (rnd() & 1)) sparkleNearEye((int)(rnd() & 1));
    }
  }
  // where the face is: in the middle (or the steps), a little closer when it is loud
  roamTo(drv > 0.f && fig == 1 ? stepX * drv : 0.f, 0.f, 1.f + 0.07f * mLoud * s);
}

static float fpsNow = 60.f;
float fps() { return fpsNow; }
void update(float dt) {
  if (dt > 0.f) fpsNow += (1.f / dt - fpsNow) * 0.05f;
  if (dt > 0.1f) dt = 0.1f;
  T += dt;
  memset(&P, 0, sizeof(P));
  for (int i = 0; i < 2; ++i)
    for (int c = 0; c < NE; ++c) P.e[i][c] = ENEU[c];
  blinkSuppress = false;

  handleTouch(dt);
  idleGaze(dt, cur >= 0 || tDown);

  // voice
  vLevel = lerpf(vLevel, vActive ? vLevelIn : 0.f, 1.f - expf(-dt * 18.f));
  vLevelSlow = lerpf(vLevelSlow, vLevel, 1.f - expf(-dt * 3.f));
  vPitch = lerpf(vPitch, vPitchIn, 1.f - expf(-dt * 6.f));
  vOnsetNow = vActive && vLevel - vLevelSlow > 0.1f && T - vOnsetT > 0.2f;
  if (vOnsetNow) vOnsetT = T;
  vQuiet = vActive ? 0.f : vQuiet + dt;
  moodS = lerpf(moodS, (moodK != MOOD_NONE && T - moodSetT < 3.5f) ? 1.f : 0.f, 1.f - expf(-dt * 4.f));
  if (wakeReq) {  // wake word: full listening session
    wakeReq = false;
    moodK = MOOD_NONE;
    moodS = 0;
    vQuiet = 0;
    rqN = 0;
    lastInteract = T;
    if (cur != B_PET) startBeh(B_LISTEN);
  }
#if CFG_WAKEWORD_ENABLE
  // the recorder decides when the utterance is over (listenDone); this is only a failsafe
  if (cur == B_LISTEN && bt > 16.f) listenDone(false);
#else
  if (vActive && cur != B_LISTEN && cur != B_PET && !tDown) startBeh(B_LISTEN);
  if (cur == B_LISTEN && vQuiet > 0.9f) startBeh(B_THINK);
#endif

  restingPose(dt, cur < 0 || behaviorKind(cur) == K_IDLE);
  if (cur >= 0) {
    const Beh& B = BEH[cur];
    bpt = bt;
    bt += dt * bSpeed[cur];
    bool done = bt >= B.dur || (cur == B_PET && !tDown) || (cur == B_THINK && !thinkWait && rqN > 0 && bt > 2.4f);
    if (cur == B_THINK && thinkWait && bt > 28.f) {  // no answer came
      const char* hm = "confused";
      react(&hm, 1);
    } else if (done) {
      if (rqN > 0 && rqI + 1 < rqN) {
        startBeh(rq[++rqI]);
      } else {
        rqN = 0;
        cur = -1;
        idleTimer = frand(L.idleMin, maxf(L.idleMin, L.idleMax)) / clampf(L.energy, 0.3f, 2.f);
      }
    } else {
      float w = env(bt, 0.f, B.attack, B.dur - B.release, B.dur) * bInt[cur];
      B.fn(bt, B.dur, w);
      flavour(cur, w);
    }
  } else if (!tDown && musicS < 0.3f) {  // (while it dances no idle moves of its own)
    idleTimer -= dt;
    if (idleTimer <= 0) pickBeh();
  }
  // nobody around for a while: fall asleep
  bool idleish = cur < 0 || behaviorKind(cur) == K_IDLE;
  // music nearby: it dances along (see groove)
  musicS = lerpf(musicS, L.music && idleish && !asleep() && !tDown ? musicIn : 0.f, 1.f - expf(-dt * 1.2f));
  if (musicS > 0.01f) {
    musicPos += dt * musicBpm / 60.f;
    float d = musicTarget - musicPos;
    d -= ffloor(d + 0.5f);  // the nearest whole-beat offset
    musicPos += d * (1.f - expf(-dt * 3.f));  // follow the microphone's beat clock smoothly
    groove(dt);
    if (musicS > 0.5f) lastInteract = T;  // no falling asleep while it dances
  }
  if (L.sleepAfter > 0 && bOn[B_FALLASLEEP] && idleish && !tDown && T - lastInteract > L.sleepAfter) {
    rq[0] = B_FALLASLEEP;
    rq[1] = B_SLEEP;
    rqN = 2;
    rqI = 0;
    startBeh(B_FALLASLEEP);
  }

  // talking: eyes bob with the syllables, look at you, smile a little
  if (spkIn >= 0.f) {
    spk = lerpf(spk, spkIn, 1.f - expf(-dt * 25.f));
    spkSlow = lerpf(spkSlow, spk, 1.f - expf(-dt * 4.f));
    spkT += dt;
    float on = sstep(0.f, 0.3f, spkT);
    if (spk - spkSlow > 0.12f && T - vOnsetT > 0.16f) {  // syllable
      vOnsetT = T;
      SG[FY].v -= 25.f + 35.f * spk;
      kickE(SY, 0.8f + 1.5f * spk);
    }
    for (int i = 0; i < 2; ++i) {
      P.e[i][SY] += 0.06f * spk * on;
      P.e[i][SX] -= 0.02f * spk * on;
      P.e[i][IS] += 0.08f * spk * on;
    }
    bool calm = cur < 0 || behaviorKind(cur) == K_IDLE;
    if (calm) {
      G(GX, 0.f, 0.75f * on);
      G(GY, -0.05f, 0.75f * on);
      E(LB, 0.18f + 0.15f * spk, on);
    }
    P.dy -= 2.f * spk * on;
  } else {
    spk = spkSlow = spkT = 0;
  }

  if (touchHold > 0) {  // look at the finger
    G(GX, touchGX, touchHold);
    G(GY, touchGY, touchHold);
  }
  // asked to look somewhere ("schau nach links"): eyes and face turn that way for a moment
  if (lookT > 0.f) {
    lookT -= dt;
    float k = sstep(0.f, 0.4f, lookT);
    G(GX, lookGX, k);
    G(GY, lookGY, k);
    roamTo(lookGX * 22.f * k, lookGY * 14.f * k, 1.f);
  }
  // wandering: when it is just being itself it picks spots now and then (and looks there first);
  // listening, talking, reactions and touch bring it back to the middle
  bool free = (cur < 0 || behaviorKind(cur) == K_IDLE) && spkIn < 0.f && !tDown;
  if (!roamHeld) {
    if (!free || L.roam <= 0.f) {
      roamTX = roamTY = 0.f;
      roamTZ = 1.f;
    } else if (cur < 0 && (roamTimer -= dt * L.energy) <= 0.f) {
      roamTimer = frand(5.f, 12.f);
      roamTZ = 1.f - L.roam * frand(0.f, 0.25f);
      float nx = frand(-0.6f, 0.6f) * roamRangeX(roamTZ), ny = frand(-0.5f, 0.5f) * roamRangeY(roamTZ);
      gzTx = clampf((nx - roamTX) / 50.f, -1.f, 1.f);
      gzTy = clampf((ny - roamTY) / 50.f, -0.7f, 0.7f);
      gzTimer = 0.9f;
      roamTX = nx;
      roamTY = ny;
    }
  }
  roamHeld = false;
  // the head follows the gaze
  P.g[FX] += P.g[GX] * 11.f * L.headFollow;
  P.g[FY] += P.g[GY] * 7.f * L.headFollow;

  glitchAmt = P.glitch;
  glitchSeedT -= dt;
  if (glitchSeedT <= 0.f) {  // new tear pattern a few times per second
    glitchSeed = rnd();
    glitchSeedT = frand(0.04f, 0.11f);
  }

  updateBlink(dt);
  updateSprites(dt);
  composeFrame(dt);
}

Rect dirtyRect() { return F.dirty; }

static RGB rgbOf(uint32_t c) { return {(float)((c >> 16) & 255), (float)((c >> 8) & 255), (float)(c & 255)}; }
void setLook(const Look& l) {
  L = l;
  RX = 50.f * clampf(l.eyeW, 0.4f, 1.5f);
  float h = clampf(l.eyeH, 0.4f, 1.5f);
  RYT = 57.f * h;
  RYB = 55.f * h;
  SEP = 57.f * clampf(l.spacing, 0.6f, 1.4f);
  CY = 118.f + clampf(l.posY, -60.f, 60.f);
  BASE_TILT = 0.085f * clampf(l.tilt, -2.f, 3.f);
  IRX = 0.47f * clampf(l.irisW, 0.3f, 1.8f);
  IRYT = 0.80f * clampf(l.irisH, 0.3f, 1.6f);
  IRYB = 0.76f * clampf(l.irisH, 0.3f, 1.6f);
  IEGG = clampf(l.irisEgg, -0.4f, 0.5f);
  STROKE = clampf(l.stroke, 2.f, 16.f);
  ENEU[HL] = sat(l.highlights);
  // the resting face's straight lids (see composeEye); together they always leave the eye half open
  L.lidTop = clampf(l.lidTop, 0.f, 0.6f);
  L.lidBottom = clampf(l.lidBottom, 0.f, minf(0.5f, 0.7f - L.lidTop));
  L.brow = clampf(l.brow, -1.f, 1.f);
  L.shape = sat(l.shape);
  L.pupil = clampf(l.pupil, 0.f, 1.f);
  L.asym = clampf(l.asym, 0.f, 0.3f);
  L.squash = clampf(l.squash, 0.f, 2.f);
  L.energy = clampf(l.energy, 0.3f, 2.f);
  L.roam = sat(l.roam);
  cwCustom.sclera = rgbOf(l.sclera);
  cwCustom.iris = rgbOf(l.iris);
  cwCustom.accent = rgbOf(l.accent);
  cwCustom.fx = rgbOf(l.sclera);
  cwCustom.hl = rgbOf(l.shine);
  RGB nb = rgbOf(l.bg);
  if (nb.r != bgCol.r || nb.g != bgCol.g || nb.b != bgCol.b) prevBox = {0, 0, W, H};  // repaint everything
  bgCol = nb;
  int want = l.colorway < 0 || l.colorway >= NCW ? NCW : l.colorway;  // NCW = custom
  if (want != cwCur) {
    cwPrev = cwCur;
    cwCur = want;
    cwT = 0;
  }
}
const Look& look() { return L; }
int colorwayCount() { return NCW; }
const char* colorwayNameAt(int i) { return cwAt(i).name; }
static uint32_t hexOf(const RGB& c) { return ((uint32_t)c.r << 16) | ((uint32_t)c.g << 8) | (uint32_t)c.b; }
uint32_t colorwayColor(int i, int which) {
  const Colorway& c = cwAt(i);
  return hexOf(which == 0 ? c.sclera : which == 1 ? c.iris : c.accent);
}
void setBehavior(int i, bool on, float speed, float intensity) {
  if (i < 0 || i >= NB) return;
  bOn[i] = on;
  bSpeed[i] = clampf(speed, 0.3f, 3.f);
  bInt[i] = clampf(intensity, 0.f, 1.f);
}
int behaviorKind(int i) {
  switch (i) {
    case B_LOOK: case B_GLANCE: case B_DRIFT: case B_FOCUS: case B_PURSUIT: case B_FALLASLEEP:
    case B_WANDER: case B_PEEK: case B_APPROACH: case B_BOUNCE: case B_SQUISH: case B_SWAY:
      return K_IDLE;
    case B_BOOP: case B_PET:
      return K_TOUCH;
    case B_WAKE: case B_LISTEN: case B_THINK: case B_HEAR: case B_SLEEP: case B_COLOR:
      return K_INTERNAL;
    case B_OFFLINE: case B_QUOTA: case B_CANT: case B_HUH: case B_LOCKED: case B_GLITCH:
      return K_ERROR;
    case B_LIGHT_ON: case B_LIGHT_OFF: case B_LIGHT_COLOR: case B_TIMER: case B_ALARM: case B_WEATHER: case B_BATTERY:
      return K_HOME;
    default:
      return K_REACTION;
  }
}
int behaviorIndex(const char* name) {
  for (int i = 0; i < NB; ++i)
    if (!strcmp(BEH[i].name, name)) return i;
  return -1;
}
int behaviorCount() { return NB; }
const char* behaviorName(int i) { return (i >= 0 && i < NB) ? BEH[i].name : "?"; }
void trigger(int i) {
  if (i < 0 || i >= NB) return;
  rqN = 0;
  lastInteract = T;
  if (i == B_FALLASLEEP) {
    rq[0] = B_FALLASLEEP;
    rq[1] = B_SLEEP;
    rqN = 2;
    rqI = 0;
  }
  thinkWait = i == B_THINK;
  startBeh(i);
}
const char* currentName() { return cur >= 0 ? BEH[cur].name : "idle"; }
const char* colorwayName() { return cwAt(cwCur).name; }

// ============================================================== renderer
#ifdef EYES_SWAP_BYTES
static inline uint16_t px565(uint16_t c) { return (uint16_t)((c >> 8) | (c << 8)); }
#else
static inline uint16_t px565(uint16_t c) { return c; }
#endif
static inline uint16_t pack(float r, float g, float b) {
  int R = (int)(r * (31.f / 255.f) + 0.5f), G_ = (int)(g * (63.f / 255.f) + 0.5f), B_ = (int)(b * (31.f / 255.f) + 0.5f);
  return px565((uint16_t)((R << 11) | (G_ << 5) | B_));
}
static inline void unpack(uint16_t c, float& r, float& g, float& b) {
  c = px565(c);
  r = (float)(c >> 11) * (255.f / 31.f);
  g = (float)((c >> 5) & 63) * (255.f / 63.f);
  b = (float)(c & 31) * (255.f / 31.f);
}
HOT static inline void blendPx(uint16_t* p, const RGB& c, float a) {
  if (a >= 0.999f) {
    *p = pack(c.r, c.g, c.b);
    return;
  }
  float r, g, b;
  unpack(*p, r, g, b);
  *p = pack(lerpf(r, c.r, a), lerpf(g, c.g, a), lerpf(b, c.b, a));
}

struct Target {
  uint16_t* buf;
  int x0, y0, x1, y1, stride;
};
static inline bool clipTo(const Rect& bb, const Target& t, int& x0, int& y0, int& x1, int& y1) {
  x0 = bb.x0 > t.x0 ? bb.x0 : t.x0;
  y0 = bb.y0 > t.y0 ? bb.y0 : t.y0;
  x1 = bb.x1 < t.x1 ? bb.x1 : t.x1;
  y1 = bb.y1 < t.y1 ? bb.y1 : t.y1;
  return x1 > x0 && y1 > y0;
}

HOT static void drawBlush(const BlushR& bl, const Target& t) {
  int x0, y0, x1, y1;
  if (bl.a < 0.01f || !clipTo(bl.bb, t, x0, y0, x1, y1)) return;
  RGB c = F.acc;
  RGB hc = mix(F.acc, RGB{255, 255, 255}, 0.55f);
  float irx = 1.f / bl.rx, iry = 1.f / bl.ry;
  for (int y = y0; y < y1; ++y) {
    uint16_t* row = t.buf + (y - t.y0) * t.stride - t.x0;
    float py = y + 0.5f - bl.cy;
    for (int x = x0; x < x1; ++x) {
      float px = x + 0.5f - bl.cx;
      float d = sdEllipse(px, py, irx, iry);
      float a = sat((1.f - d) / 2.5f) * bl.a * 0.85f;
      if (a <= 0.f) continue;
      blendPx(&row[x], c, a);
      // three little hatch strokes
      float dh = 1e9f;
      for (int k = -1; k <= 1; ++k) {
        float ox = k * 8.f;
        dh = minf(dh, sdSeg(px, py, ox - 2.2f, 3.5f, ox + 2.2f, -3.5f));
      }
      float ah = sat(1.4f - dh) * bl.a;
      if (ah > 0.f) blendPx(&row[x], hc, ah);
    }
  }
}

static inline void warpEye(const EyeR& e, float& x, float& y) {
  if (e.wobA > 0.05f) {
    float ox = x;
    x += e.wobA * fsin(y * 0.05f + e.wobPh);
    y += e.wobA * fsin(ox * 0.05f + e.wobPh * 1.3f + 1.7f);
  }
}

// signed distance to the visible sclera (egg, lids, closed-eye stroke, chevrons)
HOT static inline float evalSclera(const EyeR& e, float wx, float wy) {
  const float dx = wx - e.cx, dy = wy - e.cy;
  const float lx = dx * e.c + dy * e.s, ly = dy * e.c - dx * e.s;
  float d = e.sqK < 1.f ? sdEgg(lx, ly, e.sirx, e.siryT, e.siryB, e.egg) : 0.f;
  if (e.sqK > 0.f) {  // towards a pill / rounded rectangle
    float hy = 0.5f * (e.ryT + e.ryB);
    float db = sdBox(lx, ly - 0.5f * (e.ryB - e.ryT), e.rx, hy, minf(e.rx, hy) * e.sqR);
    d = e.sqK < 1.f ? lerpf(d, db, e.sqK) : db;
  }
  if (e.flT) d = maxf(d, (e.fT + e.fTk * lx - ly) * e.fTn);
  if (e.flB) d = maxf(d, ly - e.fB);
  if (e.lidT || e.lidB) {
    float u = lx * e.sirx;
    float ss = 1.f - u * u;
    float rs = ss > 0.0064f ? frsqrt(ss) : 12.5f;  // 1/s
    float s = ss > 0.0064f ? ss * rs : 0.08f;
    float dsdx = -lx * e.irx2 * rs;
    if (e.lidT) {
      float st = 1.f + (s - 1.f) * e.lcT;  // straighter on square eyes
      float yl = e.yT * st + e.kT * lx;
      float g = e.yT * dsdx * e.lcT + e.kT;
      d = maxf(d, (yl - ly) * frsqrt(1.f + g * g));
    }
    if (e.lidB) {
      float yl = e.yB * s + e.kB * lx;
      float g = e.yB * dsdx + e.kB;
      d = maxf(d, (ly - yl) * frsqrt(1.f + g * g));
    }
  }
  // closed eye: round-capped stroke along the lid meeting curve
  if (e.closeK > 0.f) {
    float ds;
    if (fabsf(lx) <= e.L) {
      float f = e.c0 + (e.c1 + e.c2 * lx) * lx;
      float fp = e.c1 + 2.f * e.c2 * lx;
      ds = fabsf(ly - f) * frsqrt(1.f + fp * fp) - e.hT;
    } else {
      float ex = lx > 0 ? e.L : -e.L;
      float ey = e.c0 + (e.c1 + e.c2 * ex) * ex;
      float qx = lx - ex, qy = ly - ey;
      ds = fsqrt(qx * qx + qy * qy) - e.hT;
    }
    d = lerpf(d, ds, e.closeK);
  }
  // > < squeeze
  if (e.chev > 0.f) {
    float cx = lx * e.chevDir;
    float d1 = sdSegK(cx, ly, -e.chW, -e.chH, e.chBx, e.chBy, e.chIbb);
    float d2 = sdSegK(cx, ly, -e.chW, e.chH, e.chBx, -e.chBy, e.chIbb);
    d = lerpf(d, minf(d1, d2) - e.chT, e.chev);
  }
  return d;
}

// symbol in the iris (x right, y up, unit ~1): main shape dm, accent part da (battery charge)
HOT static inline void sdGlyph(float x, float y, float& dm, float& da) {
  da = 1e9f;
  switch (F.glyph) {
    case GL_WIFI: {  // dot + two arcs that switch on and off
      float oy = y + 0.55f;
      float dot = fsqrt(x * x + oy * oy) - 0.21f;
      float v1 = F.gp[0], v2 = F.gp[1];
      float a1 = sdArcUp(x, oy, 0.7071f, 0.7071f, 0.66f) - 0.13f * v1 + (1.f - v1) * 0.4f;
      float a2 = sdArcUp(x, oy, 0.7071f, 0.7071f, 1.14f) - 0.13f * v2 + (1.f - v2) * 0.4f;
      dm = minf(dot, minf(a1, a2));
      return;
    }
    case GL_BATT: {  // outline + nub, the charge level is the accent part
      float body = fabsf(sdBox(x + 0.08f, y, 0.8f, 0.48f, 0.16f)) - 0.085f;
      float nub = sdBox(x - 0.86f, y, 0.07f, 0.2f, 0.05f);
      dm = minf(body, nub);
      float hw = maxf(0.06f, 0.615f * F.gp[0]);
      da = sdBox(x + 0.695f - hw, y, hw, 0.295f, 0.05f) + (1.f - F.gp[1]) * 0.6f;
      return;
    }
    case GL_QUES:
      dm = sdQues(x * 1.05f, -y * 1.05f) / 1.05f;
      return;
    case GL_NOPE: {  // no-entry sign
      float ring = fabsf(fsqrt(x * x + y * y) - 0.8f) - 0.13f;
      float bar = sdSeg(x, y, -0.56f, 0.56f, 0.56f, -0.56f) - 0.13f;
      dm = minf(ring, bar);
      return;
    }
    case GL_BULB: {  // glass (open at the bottom) + neck + screw base; the glow and the rays are the accent
      float gy = y - 0.22f;
      float rr = fsqrt(x * x + gy * gy);
      float glass = maxf(fabsf(rr - 0.6f) - 0.1f, -(y + 0.2f));
      float neck = minf(sdSeg(x, y, -0.43f, -0.2f, -0.22f, -0.42f), sdSeg(x, y, 0.43f, -0.2f, 0.22f, -0.42f)) - 0.1f;
      float base = minf(sdSeg(x, y, -0.22f, -0.55f, 0.22f, -0.55f), sdSeg(x, y, -0.15f, -0.74f, 0.15f, -0.74f)) - 0.08f;
      dm = minf(glass, minf(neck, base));
      float glow = rr - 0.46f + (1.f - F.gp[0]) * 0.6f;
      float rays = 1e9f;
      if (F.gp[1] > 0.f)
        for (int k = 0; k < 5; ++k) {  // sin / cos of -1.2 -0.6 0 0.6 1.2 rad
          static const float S[5] = {-0.932f, -0.565f, 0.f, 0.565f, 0.932f}, C[5] = {0.362f, 0.825f, 1.f, 0.825f, 0.362f};
          rays = minf(rays, sdSeg(x, gy, 0.82f * S[k], 0.82f * C[k], 1.02f * S[k], 1.02f * C[k]) - 0.07f);
        }
      da = minf(glow, rays + (1.f - F.gp[1]) * 0.4f);
      return;
    }
    case GL_CLOCK: {  // ring + two hands; alarm: two bells on top (accent)
      float rr = fsqrt(x * x + y * y);
      float ring = fabsf(rr - 0.72f) - 0.1f;
      float m = sdSeg(x, y, 0.f, 0.f, 0.52f * F.tr[0], 0.52f * F.tr[1]) - 0.08f;
      float h = sdSeg(x, y, 0.f, 0.f, 0.34f * F.tr[2], 0.34f * F.tr[3]) - 0.09f;
      dm = minf(ring, minf(m, h));
      da = 1e9f;
      if (F.gp[2] > 0.5f) {
        float bl = fsqrt((x + 0.55f) * (x + 0.55f) + (y - 0.82f) * (y - 0.82f)) - 0.24f;
        float br = fsqrt((x - 0.55f) * (x - 0.55f) + (y - 0.82f) * (y - 0.82f)) - 0.24f;
        da = maxf(minf(bl, br), -(y - 0.74f));  // half discs
      }
      return;
    }
    case GL_WEATHER: {
      int c = (int)(F.gp[2] + 0.5f);
      float ph = F.gp[0];
      auto cloud = [](float x, float y) {
        float a = fsqrt((x + 0.38f) * (x + 0.38f) + (y + 0.05f) * (y + 0.05f)) - 0.34f;
        float b = fsqrt((x - 0.05f) * (x - 0.05f) + (y - 0.18f) * (y - 0.18f)) - 0.44f;
        float d = fsqrt((x - 0.45f) * (x - 0.45f) + (y + 0.08f) * (y + 0.08f)) - 0.3f;
        return minf(minf(a, b), minf(d, sdBox(x + 0.02f, y + 0.22f, 0.5f, 0.17f, 0.1f)));
      };
      auto sun = [&](float x, float y, float r) {
        float d = fsqrt(x * x + y * y) - r;
        for (int k = 0; k < 8; ++k) {  // rays turning slowly
          float sx = F.tr[4 + 2 * k], cy = F.tr[5 + 2 * k];
          d = minf(d, sdSeg(x, y, (r + 0.14f) * sx, (r + 0.14f) * cy, (r + 0.34f) * sx, (r + 0.34f) * cy) - 0.07f);
        }
        return d;
      };
      dm = da = 1e9f;
      if (c == W_SUN) {
        da = sun(x, y, 0.42f);
      } else if (c == W_NIGHT) {  // crescent moon
        float a = fsqrt(x * x + y * y) - 0.62f;
        float b = fsqrt((x - 0.32f) * (x - 0.32f) + (y - 0.22f) * (y - 0.22f)) - 0.5f;
        da = maxf(a, -b);
      } else if (c == W_FOG) {
        for (int k = 0; k < 3; ++k) {
          float yy = y - 0.4f + k * 0.4f + 0.06f * fsin(x * 4.f + ph * 3.f + k);
          dm = minf(dm, maxf(fabsf(yy) - 0.08f, fabsf(x + (k == 1 ? 0.12f : 0.f)) - 0.62f));
        }
      } else {
        float cy = c == W_PARTLY || c == W_CLOUD ? y + 0.1f : y - 0.22f;
        dm = cloud(x, cy);
        if (c == W_PARTLY) da = maxf(sun(x - 0.38f, y - 0.42f, 0.3f), -(dm - 0.1f));  // peeks out behind
        float fall = ph * 1.6f;
        for (int k = 0; k < 3 && (c == W_RAIN || c == W_SNOW); ++k) {  // drops / flakes falling out of it
          float f = fall + k * 0.37f;
          f -= ffloor(f);
          float px = -0.4f + k * 0.4f, py = -0.3f - 0.65f * f;
          float d = c == W_RAIN ? sdSeg(x, y, px + 0.05f, py + 0.12f, px - 0.05f, py - 0.12f) - 0.07f
                                : fsqrt((x - px) * (x - px) + (y - py) * (y - py)) - 0.1f;
          da = minf(da, d + (f > 0.85f ? (f - 0.85f) * 3.f : 0.f));
        }
        if (c == W_STORM)  // lightning bolt
          da = minf(sdSeg(x, y, 0.1f, -0.25f, -0.12f, -0.6f), sdSeg(x, y, -0.12f, -0.6f, 0.12f, -0.6f)) - 0.07f;
        if (c == W_STORM) da = minf(da, sdSeg(x, y, 0.12f, -0.6f, -0.08f, -0.98f) - 0.07f);
      }
      return;
    }
    case GL_GAUGE: {  // usage meter: an arc, the needle (angle from 12 o'clock in tr 0/1), the red end is the accent
      float rr = fsqrt(x * x + y * y);
      float arc = maxf(fabsf(rr - 0.82f) - 0.1f, -(y + 0.28f));
      float needle = sdSeg(x, y, 0.f, -0.02f, 0.66f * F.tr[0], 0.66f * F.tr[1] - 0.02f) - 0.075f;
      float hub = fsqrt(x * x + (y + 0.02f) * (y + 0.02f)) - 0.17f;
      dm = minf(maxf(arc, x - 0.42f), minf(needle, hub));
      da = maxf(arc, 0.48f - x);
      return;
    }
    case GL_LOCK: {  // padlock with keyhole, the shackle lifts a little when tugged
      float body = sdBox(x, y + 0.32f, 0.66f, 0.46f, 0.14f);
      float hole = minf(fsqrt(x * x + (y + 0.24f) * (y + 0.24f)) - 0.13f, sdSeg(x, y, 0.f, -0.3f, 0.f, -0.56f) - 0.06f);
      body = maxf(body, -hole);
      float sy = y - (0.14f + 0.3f * F.gp[0]);
      float sh = minf(sdSeg(x, sy, 0.4f, 0.f, 0.4f, -0.34f), sdSeg(x, sy, -0.4f, 0.f, -0.4f, -0.34f));
      if (sy >= 0.f) sh = minf(sh, fabsf(fsqrt(x * x + sy * sy) - 0.4f));
      dm = minf(body, sh - 0.11f);
      return;
    }
  }
  dm = 1e9f;
}

// signed distances to the iris, its highlights and the accent part of a symbol (1e9 = nowhere near)
HOT static inline void evalIris(const EyeR& e, float wx, float wy, float& di, float& dh, float& da) {
  di = dh = da = 1e9f;
  if (e.irisA <= 0.f) return;
  float qx = wx - e.ix, qy = wy - e.iy;
  const bool gl = e.glyph > 0.f;
  if (!gl && (fabsf(qx) >= e.ibR || fabsf(qy) >= e.ibR)) return;
  float u = e.m00 * qx + e.m01 * qy, v = e.m10 * qx + e.m11 * qy;
  di = e.sqK < 1.f ? sdEgg(u, v, e.iirx, e.iiryT, e.iiryB, e.iegg) : 0.f;
  if (e.sqK > 0.f) {
    float hy = 0.5f * (e.iryT + e.iryB);
    float db = sdBox(u, v - 0.5f * (e.iryB - e.iryT), e.irx, hy, minf(e.irx, hy) * e.isqR);
    di = e.sqK < 1.f ? lerpf(di, db, e.sqK) : db;
  }
  if (e.pr > 0.f && !gl) da = maxf(fsqrt(u * u + v * v) - e.pr, di + 1.5f);  // the pupil (accent part), inside the iris
  if (e.heart > 0.f) di = lerpf(di, sdHeart(u * e.ihs, -v * e.ihs + 0.55f) * e.hs, e.heart);
  if (e.star > 0.f) {
    float su = u * e.starC + v * e.starS, sv = v * e.starC - u * e.starS;
    di = lerpf(di, sdStar(su, -sv, e.sr - 4.f, STAR5) - 4.f, e.star);
  }
  if (gl) {
    float px = wx - e.gcx, py = wy - e.gcy;
    float rr = (px * px + py * py) * e.igs * e.igs;
    if (rr > 2.56f) {  // every symbol fits into radius 1.35: beyond 1.6 the distance to that circle is a bound
      float d = (fsqrt(rr) - 1.35f) * e.gs;
      di = lerpf(di, d, e.glyph);
      da = d + 2.f * e.gs;
      goto shine;
    }
    float lx = px * e.c + py * e.s, ly = py * e.c - px * e.s;  // eye-local, y down
    float gx = (lx * e.grc + ly * e.grs) * e.igs, gy = -(ly * e.grc - lx * e.grs) * e.igs;
    float dm, dac;
    sdGlyph(gx, gy, dm, dac);
    di = lerpf(di, dm * e.gs, e.glyph);
    da = (dac + 2.f * (1.f - e.glyph)) * e.gs;  // the accent only shows once the symbol has formed
  }
shine:
  if (e.hl > 0.01f) {
    float ax = wx - e.h1x, ay = wy - e.h1y, bx = wx - e.h2x, by = wy - e.h2y;
    dh = minf(fsqrt(ax * ax + ay * ay) - e.h1r, fsqrt(bx * bx + by * by) - e.h2r);
  }
}

struct EyePal {
  RGB scl, iris, hl, acc;
  uint16_t sclPx, irisPx;
};
HOT static inline void shadeEye(uint16_t* p, const EyeR& e, const EyePal& pal, float cov, float di, float dh,
                                float da = 1e9f) {
  float ci = sat(0.5f - di) * e.irisA;
  float ch = sat(0.5f - dh) * ci;
  float ca = sat(0.5f - da) * e.irisA;
  if (ca > 0.f) {
    RGB c = mix(mix(pal.scl, pal.iris, ci), pal.acc, ca);
    if (ch > 0.f) c = mix(c, pal.hl, ch);  // the shine stays on top of a pupil
    blendPx(p, c, cov);
    return;
  }
  if (cov >= 1.f && ch <= 0.f) {
    if (ci <= 0.f) {
      *p = pal.sclPx;
      return;
    }
    if (ci >= 0.999f) {
      *p = pal.irisPx;
      return;
    }
  }
  RGB c = mix(pal.scl, pal.iris, ci);
  if (ch > 0.f) c = mix(c, pal.hl, ch);
  blendPx(p, c, cov);
}

// 4x4 blocks. The distance at the block centre bounds the distance at every pixel
// of the block (fields are ~1-Lipschitz; e.Ms / e.Mi add headroom for wobble and
// iris stretch). Sclera and iris are classified separately, so a block crossed by
// only one outline evaluates only that outline per pixel.
// glitch: rows are torn sideways in 6 px slices (some slices only, new pattern every few frames)
HOT static inline float glitchOff(int y) {
  uint32_t h = (uint32_t)(y / 6) * 2654435761u ^ F.glitchSeed;
  h ^= h >> 13;
  h *= 0x5bd1e995u;
  h ^= h >> 15;
  if ((h & 7) > 2) return 0.f;
  return F.glitch * ((float)((h >> 8) & 255) * (1.f / 127.5f) - 1.f);
}

// per pixel, no block classification (rows are shifted individually)
HOT static void drawEyeGlitch(const EyeR& e, const Target& t, const EyePal& pal, int x0, int y0, int x1, int y1) {
  for (int y = y0; y < y1; ++y) {
    uint16_t* row = t.buf + (y - t.y0) * t.stride - t.x0;
    float off = glitchOff(y);
    for (int x = x0; x < x1; ++x) {
      float wx = x + 0.5f - off, wy = y + 0.5f;
      warpEye(e, wx, wy);
      float cov = 0.5f - evalSclera(e, wx, wy);
      if (cov <= 0.f) continue;
      if (cov > 1.f) cov = 1.f;
      float di, dh, da;
      evalIris(e, wx, wy, di, dh, da);
      shadeEye(&row[x], e, pal, cov, di, dh, da);
    }
  }
}

HOT static void drawEye(const EyeR& e, const Target& t) {
  int x0, y0, x1, y1;
  if (!clipTo(e.bb, t, x0, y0, x1, y1)) return;
  EyePal pal = {F.scl, F.iris, F.hl, e.glyph <= 0.f ? F.pup : F.glyph >= GL_BULB || F.glyph == GL_BATT ? F.gcol : F.acc, pack(F.scl.r, F.scl.g, F.scl.b), pack(F.iris.r, F.iris.g, F.iris.b)};
  if (F.glitch > 0.f) {
    drawEyeGlitch(e, t, pal, x0, y0, x1, y1);
    return;
  }
  const float Ms = e.Ms, Mi = e.Mi;
  for (int by = y0; by < y1; by += 4) {
    int by1 = by + 4 < y1 ? by + 4 : y1;
    for (int bx = x0; bx < x1; bx += 4) {
      int bx1 = bx + 4 < x1 ? bx + 4 : x1;
      float cx = 0.5f * (bx + bx1), cy = 0.5f * (by + by1);
      warpEye(e, cx, cy);
      float ds = evalSclera(e, cx, cy);
      if (ds > Ms) continue;
      float di, dh, da;
      evalIris(e, cx, cy, di, dh, da);
      const bool sclIn = ds < -Ms;
      const bool irisU = (di > Mi || di < -Mi) && (dh > Mi || dh < -Mi) && (da > Mi || da < -Mi);
      const float diB = di < 0 ? -9.f : 9.f, dhB = dh < 0 ? -9.f : 9.f, daB = da < 0 ? -9.f : 9.f;
      if (sclIn && irisU) {
        uint16_t v = 0;
        shadeEye(&v, e, pal, 1.f, diB, dhB, daB);
        for (int y = by; y < by1; ++y) {
          uint16_t* row = t.buf + (y - t.y0) * t.stride - t.x0;
          for (int x = bx; x < bx1; ++x) row[x] = v;
        }
        continue;
      }
      for (int y = by; y < by1; ++y) {
        uint16_t* row = t.buf + (y - t.y0) * t.stride - t.x0;
        for (int x = bx; x < bx1; ++x) {
          float wx = x + 0.5f, wy = y + 0.5f;
          warpEye(e, wx, wy);
          float cov = 1.f;
          if (!sclIn) {
            cov = 0.5f - evalSclera(e, wx, wy);
            if (cov <= 0.f) continue;
            if (cov > 1.f) cov = 1.f;
          }
          if (irisU) {
            shadeEye(&row[x], e, pal, cov, diB, dhB, daB);
          } else {
            evalIris(e, wx, wy, di, dh, da);
            shadeEye(&row[x], e, pal, cov, di, dh, da);
          }
        }
      }
    }
  }
}

HOT static float sdSprite(int type, float u, float v) {
  switch (type) {
    case SP_HEART:
      return sdHeart(u * 0.62f, -v * 0.62f + 0.55f) / 0.62f;
    case SP_Z: {
      float d = sdSeg(u, v, -0.62f, -0.62f, 0.62f, -0.62f);
      d = minf(d, sdSeg(u, v, 0.62f, -0.62f, -0.62f, 0.62f));
      d = minf(d, sdSeg(u, v, -0.62f, 0.62f, 0.62f, 0.62f));
      return d - 0.17f;
    }
    case SP_SPARK:
      return sdStar(u, v, 1.f, STAR4);
    case SP_EXCL: {
      float d = sdSeg(u, v, 0.f, -0.9f, 0.f, 0.22f) - 0.21f;
      float dx = u, dy = v - 0.72f;
      return minf(d, fsqrt(dx * dx + dy * dy) - 0.2f);
    }
    case SP_QUES:
      return sdQues(u, v);
    case SP_TEAR: {
      // uneven capsule (round bottom, pointy top)
      float px = fabsf(u), py = -v + 0.42f;  // y up, bottom circle at origin
      const float r1 = 0.55f, r2 = 0.05f, h = 1.25f;
      const float bb = (r1 - r2) / h, aa = fsqrt(1.f - bb * bb);
      float k = -bb * px + aa * py;
      if (k < 0.f) return fsqrt(px * px + py * py) - r1;
      if (k > aa * h) return fsqrt(px * px + (py - h) * (py - h)) - r2;
      return aa * px + bb * py - r1;
    }
    case SP_WAVE: {
      // arc ")" around +x, aperture +-50deg (iq sdArc with axes swapped)
      float px = fabsf(v), py = u;
      const float sx = 0.76604f, sy = 0.64279f;
      float d = (sy * px > sx * py) ? fsqrt((px - sx) * (px - sx) + (py - sy) * (py - sy))
                                    : fabsf(fsqrt(px * px + py * py) - 1.f);
      return d - 0.11f;
    }
    case SP_DOT:
      return fsqrt(u * u + v * v) - 1.f;
    case SP_ANGER: {
      float px = fabsf(u), py = fabsf(v);
      float qx = px - 0.95f, qy = py - 0.95f;
      float l = fsqrt(qx * qx + qy * qy);
      float cs = -(qx + qy) * 0.70710678f / (l + 1e-6f);
      float d;
      if (cs > 0.77f) {
        d = fabsf(l - 0.62f);
      } else {
        // arc end points at +-40deg around (-1,-1)/sqrt2
        float e1x = 0.95f - 0.62f * 0.98481f, e1y = 0.95f - 0.62f * 0.17365f;
        float e2x = 0.95f - 0.62f * 0.17365f, e2y = 0.95f - 0.62f * 0.98481f;
        float d1 = fsqrt((px - e1x) * (px - e1x) + (py - e1y) * (py - e1y));
        float d2 = fsqrt((px - e2x) * (px - e2x) + (py - e2y) * (py - e2y));
        d = minf(d1, d2);
      }
      return d - 0.13f;
    }
  }
  return 1e9f;
}

HOT static void drawSprite(const SprR& s, const Target& t) {
  int x0, y0, x1, y1;
  if (s.a < 0.01f || !clipTo(s.bb, t, x0, y0, x1, y1)) return;
  float isc = 1.f / s.sc;
  for (int y = y0; y < y1; ++y) {
    uint16_t* row = t.buf + (y - t.y0) * t.stride - t.x0;
    float py = y + 0.5f - s.y;
    for (int x = x0; x < x1; ++x) {
      float px = x + 0.5f - s.x;
      float u = (px * s.c + py * s.s) * isc, v = (py * s.c - px * s.s) * isc;
      float d = sdSprite(s.type, u, v) * s.sc;
      float a = sat(0.5f - d) * s.a;
      if (a > 0.f) blendPx(&row[x], s.col, a);
    }
  }
}

HOT void render(uint16_t* buf, int x0, int y0, int x1, int y1) {
  Target t = {buf, x0, y0, x1, y1, x1 - x0};
  if (F.bg.r + F.bg.g + F.bg.b < 1.f) {
    memset(buf, 0, (size_t)(x1 - x0) * (y1 - y0) * 2);
  } else {
    uint16_t b = pack(F.bg.r, F.bg.g, F.bg.b);
    for (int i = 0, n = (x1 - x0) * (y1 - y0); i < n; ++i) buf[i] = b;
  }
  for (int i = 0; i < 2; ++i) drawBlush(F.blush[i], t);
  for (int i = 0; i < 2; ++i) drawEye(F.eye[i], t);
  for (int i = 0; i < F.nspr; ++i) drawSprite(F.spr[i], t);
}

#ifdef ESP_PLATFORM
}  // namespace eyes
#include <stdio.h>
#include "esp_cpu.h"
namespace eyes {
static volatile float sink;
void bench() {
  const int N = 20000;
  float acc = 0;
  uint32_t c0, c1;
#define BENCH(name, expr)                       \
  c0 = esp_cpu_get_cycle_count();               \
  for (int i = 0; i < N; ++i) {                 \
    float x = 1.f + (float)(i & 255) * 0.37f;   \
    float y = -20.f + (float)(i & 127) * 0.31f; \
    acc += (expr);                              \
  }                                             \
  c1 = esp_cpu_get_cycle_count();               \
  printf("bench %-12s %6.1f cycles\n", name, (float)(c1 - c0) / N);
  BENCH("loop", x + y);
  BENCH("mul-add", x * y + x);
  BENCH("div", y / x);
  BENCH("sqrtf", sqrtf(x));
  BENCH("frsqrt", frsqrt(x));
  BENCH("frcp", frcp(x));
  BENCH("sdEllipse", sdEllipse(x, y, 0.02f, 0.018f));
  BENCH("sdEgg", sdEgg(x, y, 0.02f, 0.018f, 0.019f, 0.05f));
  float di, dh, da;
  const EyeR& e = F.eye[0];
  BENCH("evalSclera", evalSclera(e, e.cx + x * 0.1f, e.cy + y));
  BENCH("evalIris", (evalIris(e, e.cx + x * 0.1f, e.cy + y, di, dh, da), di));
  uint16_t px = 0;
  BENCH("blendPx", (blendPx(&px, F.scl, 0.5f), (float)px));
  sink = acc;
}
#endif
}  // namespace eyes
