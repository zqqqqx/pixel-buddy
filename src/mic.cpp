// ES8311 microphone (I2S slave, 16 kHz / 16 bit) + voice activity detection.
// Register sequence follows espressif/esp-bsp components/es8311 (MCLK = 256 * fs).
#include "mic.h"
#include "config.h"

#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/stream_buffer.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "kws.h"
#include "beat.h"
#if CFG_WAKEWORD_ENABLE
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "model_path.h"
#endif

namespace mic {

static const char* TAG = "mic";
// pins: xiaozhi-esp32 boards/spotpear/sp-esp32-s3-1.54-muma/config.h
static const gpio_num_t PIN_MCLK = GPIO_NUM_16, PIN_WS = GPIO_NUM_45, PIN_BCLK = GPIO_NUM_9, PIN_DIN = GPIO_NUM_10;
static const gpio_num_t PIN_SDA = GPIO_NUM_15, PIN_SCL = GPIO_NUM_14;
static const int RATE = 16000, FRAME = 320;  // 20 ms analysis frames

static i2c_master_dev_handle_t codec;
static i2s_chan_handle_t rx, tx;
static const gpio_num_t PIN_DOUT = GPIO_NUM_8, PIN_PA = GPIO_NUM_46;
static volatile State st = {false, 0, 0.5f, -90, -60};
static volatile bool debug = false;
static volatile int wakeCount = 0;
static volatile bool fakeWakeReq = false;  // serial 'w': behaves like a detection (incl. the recording)
static volatile bool wakeOn = true;
static volatile bool useEsp = true, usePixel = true;  // which wake words are active
static int16_t* volatile capBuf = nullptr;  // recording for the wake word dataset
static volatile int capN = 0, capGot = 0;
static const int PRE = RATE * 2;  // the last 2 s: pre-roll of recordings (0.5 s) and the wake word clip
// every activation is recorded: the 2 s up to the wake word and the 4 s after it. If nobody speaks, the
// session keeps it as a false wake-up (dataset / negative tests).
static const int CLIP_POST = RATE * 4, CLIP = PRE + CLIP_POST;
static int16_t* clip = nullptr;
static volatile int clipN = 0, clipPost = 0;
static volatile bool clipPixel = false;  // the last activation was "Pixel" (not "Hallo ESP")
static void grabClip(bool pixel);
static int16_t* preRing = nullptr;
static volatile uint32_t preW = 0;
bool uplinkMuted();
// speaker / uplink state (used by the analysis task and the player)
static volatile float spkLevel = 0;
static volatile int64_t lastLoudUs = -10000000;  // last time audible speech went to the speaker
static volatile bool loudAhead = false;          // audible speech is queued
static StreamBufferHandle_t up = nullptr;
static volatile bool upOn = false;
EXT_RAM_BSS_ATTR static Beat beat;  // music nearby (the robot's own voice and conversations do not count)
static volatile float beatConf = 0, beatBpm = 120, beatPos = 0, beatLoud = 0, beatDrive = 0;

#if CFG_WAKEWORD_ENABLE
static const esp_wn_iface_t* wn = nullptr;
static model_iface_data_t* wnData = nullptr;
static int wnChunk = 0;
static int16_t* wnBuf = nullptr;
static int wnFill = 0;

static srmodel_list_t* models = nullptr;
static volatile bool wnStopped = false;
static void wakeInit(const char* model) {
  models = esp_srmodel_init("model");
  if (!model) return;  // Pixel's own wake word: no WakeNet (saves memory and time)
  char* name = nullptr;
  for (int i = 0; models && i < models->num && !name; ++i)
    if (!strcmp(models->model_name[i], model)) name = models->model_name[i];
  if (!name) {
    ESP_LOGW(TAG, "wake word model %s is not in the 'model' partition", model);
    return;
  }
  wn = esp_wn_handle_from_name(name);
  wnData = wn ? wn->create(name, CFG_WAKEWORD_AGGRESSIVE ? DET_MODE_95 : DET_MODE_90) : nullptr;
  if (!wnData) {
    ESP_LOGW(TAG, "wake word model %s failed to load", name);
    wn = nullptr;
    return;
  }
  wnChunk = wn->get_samp_chunksize(wnData);
  wnBuf = (int16_t*)malloc(wnChunk * sizeof(int16_t));
  ESP_LOGI(TAG, "wake word ready: %s (%s), chunk %d", name, wn->get_word_name(wnData, 1), wnChunk);
}
static void wakeFeed(int16_t s) {
  if (!wn || wnStopped) return;
  wnBuf[wnFill++] = s;
  if (wnFill < wnChunk) return;
  wnFill = 0;
  if (!wakeOn) return;
  if (useEsp && wn->detect(wnData, wnBuf) == WAKENET_DETECTED) {
    grabClip(false);
    wakeCount = wakeCount + 1;
    printf("wake word detected!\n");
  }
}
#else
static void wakeInit(const char*) {}
static void wakeFeed(int16_t) {}
#endif

static esp_err_t wr(uint8_t reg, uint8_t val) {
  uint8_t b[2] = {reg, val};
  return i2c_master_transmit(codec, b, 2, 50);
}
static uint8_t rd(uint8_t reg) {
  uint8_t v = 0;
  i2c_master_transmit_receive(codec, &reg, 1, &v, 1, 50);
  return v;
}

static bool codecInit() {
  i2c_master_bus_handle_t bus;
  i2c_master_bus_config_t bc = {};
  bc.i2c_port = I2C_NUM_0;
  bc.sda_io_num = PIN_SDA;
  bc.scl_io_num = PIN_SCL;
  bc.clk_source = I2C_CLK_SRC_DEFAULT;
  bc.glitch_ignore_cnt = 7;
  bc.flags.enable_internal_pullup = 1;
  if (i2c_new_master_bus(&bc, &bus) != ESP_OK) return false;
  i2c_device_config_t dc = {};
  dc.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  dc.device_address = 0x18;
  dc.scl_speed_hz = 100000;
  if (i2c_master_bus_add_device(bus, &dc, &codec) != ESP_OK) return false;

  if (wr(0x00, 0x1F) != ESP_OK) return false;  // reset
  vTaskDelay(pdMS_TO_TICKS(20));
  wr(0x00, 0x00);
  wr(0x00, 0x80);                    // power on, slave mode
  wr(0x01, 0x3F);                    // all clocks on, MCLK from pin
  wr(0x02, rd(0x02) & 0x07);         // pre_div 1, pre_multi 1x (4.096 MHz MCLK, 16 kHz)
  wr(0x03, 0x10);                    // single speed, ADC OSR
  wr(0x04, 0x10);                    // DAC OSR
  wr(0x05, 0x00);                    // ADC / DAC clock dividers 1
  wr(0x06, (rd(0x06) & 0xC0) | 0x03);  // BCLK divider, not inverted
  wr(0x07, rd(0x07) & 0xC0);         // LRCK divider high
  wr(0x08, 0xFF);                    // LRCK divider low
  wr(0x09, 0x0C);                    // SDP in: I2S 16 bit
  wr(0x0A, 0x0C);                    // SDP out: I2S 16 bit
  wr(0x0D, 0x01);                    // power up analog
  wr(0x0E, 0x02);                    // enable PGA + ADC modulator
  wr(0x12, 0x00);
  wr(0x13, 0x10);
  wr(0x1C, 0x6A);                    // ADC EQ bypass, DC offset cancel
  wr(0x37, 0x08);
  wr(0x17, 0xC8);                    // ADC volume
  wr(0x14, 0x1A);                    // analog mic, PGA +30 dB
  wr(0x16, 0x03);                    // ADC digital gain +18 dB
  // DAC volume: 0x32 = 0xBF is 0 dB, 0.5 dB per step; 100 % -> 0 dB, 0 % -> -40 dB
  wr(0x32, (uint8_t)(0xBF - (100 - CFG_SPEAKER_VOLUME) * 0.8f));
  wr(0x31, 0x00);                    // DAC unmuted
  return true;
}

static bool i2sInit() {
  i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  cc.auto_clear_before_cb = true;  // silence when nothing is being played
  if (i2s_new_channel(&cc, &tx, &rx) != ESP_OK) return false;  // full duplex
  i2s_std_config_t sc = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(RATE),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
      .gpio_cfg = {},
  };
  sc.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
  sc.gpio_cfg.mclk = PIN_MCLK;
  sc.gpio_cfg.bclk = PIN_BCLK;
  sc.gpio_cfg.ws = PIN_WS;
  sc.gpio_cfg.dout = PIN_DOUT;
  sc.gpio_cfg.din = PIN_DIN;
  if (i2s_channel_init_std_mode(tx, &sc) != ESP_OK) return false;
  if (i2s_channel_init_std_mode(rx, &sc) != ESP_OK) return false;
  return i2s_channel_enable(tx) == ESP_OK && i2s_channel_enable(rx) == ESP_OK;
}

// Voice activity: energy well above an adaptive noise floor, most of it in the
// 300-3400 Hz speech band, plausible zero-crossing rate; majority vote over
// 8 frames plus a hangover so syllable gaps do not drop out.
static void task(void*) {
  static int16_t buf[FRAME * 2];
  float hpX = 0, hpY = 0, lp1 = 0, lp2 = 0;
  const float a1 = 1.f - expf(-2.f * 3.14159f * 300.f / RATE);
  const float a2 = 1.f - expf(-2.f * 3.14159f * 3400.f / RATE);
  float noise = -60.f, level = 0.f, pitch = 0.5f;
  uint8_t hist = 0;
  int hang = 0, frames = 0;
  bool voice = false;
  float eL = 0, eR = 0;  // which slot carries the microphone
  for (;;) {
    size_t got = 0;
    if (i2s_channel_read(rx, buf, sizeof(buf), &got, pdMS_TO_TICKS(200)) != ESP_OK || got == 0) continue;
    int n = got / 4;
    float e = 0, eb = 0;
    int zc = 0;
    float prev = 0;
    float fl = 0, fr = 0;
    for (int i = 0; i < n; ++i) {
      fl += (float)buf[2 * i] * buf[2 * i];
      fr += (float)buf[2 * i + 1] * buf[2 * i + 1];
    }
    eL += (fl - eL) * 0.05f;
    eR += (fr - eR) * 0.05f;
    const int ch = eL >= eR ? 0 : 1;
    for (int i = 0; i < n; ++i) {
      int16_t raw = buf[2 * i + ch];
      wakeFeed(raw);
      float x = (float)raw * (1.f / 32768.f);
      float y = x - hpX + 0.995f * hpY;  // DC blocker
      hpX = x;
      hpY = y;
      lp1 += a1 * (y - lp1);
      lp2 += a2 * (y - lp2);
      float band = lp2 - lp1;
      e += y * y;
      eb += band * band;
      if ((y > 0) != (prev > 0)) zc++;
      prev = y;
    }
    float db = 10.f * log10f(e / n + 1e-12f);
    float ratio = eb / (e + 1e-12f);
    float zcr = (float)zc / n;
    // noise floor: drops fast, rises slowly (~6 s)
    if (db < noise)
      noise += (db - noise) * 0.2f;
    else
      noise += (db - noise) * 0.003f;
    float snr = db - noise;
    bool voiced = snr > CFG_VOICE_SNR_DB && db > -66.f && ratio > 0.22f && zcr > 0.01f && zcr < 0.42f;
    hist = (uint8_t)((hist << 1) | (voiced ? 1 : 0));
    int cnt = __builtin_popcount(hist);
    if (cnt >= 3) {
      voice = true;
      hang = 20;  // 400 ms
    } else if (hang > 0) {
      hang--;
    } else {
      voice = false;
    }
    float target = voiced ? fminf(1.f, fmaxf(0.f, (snr - 8.f) / 28.f)) : 0.f;
    level += (target - level) * (target > level ? 0.6f : 0.25f);
    if (voiced) pitch += (fminf(1.f, fmaxf(0.f, (zcr - 0.03f) / 0.2f)) - pitch) * 0.3f;
    if (preRing)
      for (int i = 0; i < n; ++i) preRing[(preW + i) % PRE] = buf[2 * i + ch];
    preW = preW + n;
    if (fakeWakeReq) {
      fakeWakeReq = false;
      grabClip(true);
      wakeCount = wakeCount + 1;
    }
    if (clipPost > 0) {  // what follows the wake word
      int m = n < clipPost ? n : clipPost;
      for (int i = 0; i < m; ++i) clip[clipN + i] = buf[2 * i + ch];
      clipN = clipN + m;
      clipPost = clipPost - m;
    }
    if (capBuf && capGot < capN) {  // wake word dataset recording
      int m = n < capN - capGot ? n : capN - capGot;
      for (int i = 0; i < m; ++i) capBuf[capGot + i] = buf[2 * i + ch];
      capGot = capGot + m;
    }
    // own wake word network ("Pixel"), fed with the same microphone channel. Its log-mel features also feed
    // the music detection (spectral flux), so they run whenever no conversation does.
    static bool kwsWasOn = false;
    bool detect = usePixel && wakeOn;
    if (!upOn) {
      if (detect && !kwsWasOn) kws::init();  // start from a clean window (no audio from before the session)
      kwsWasOn = detect;
      static int16_t kmono[FRAME];
      for (int i = 0; i < n; ++i) kmono[i] = buf[2 * i + ch];
      if (kws::feed(kmono, n, detect)) {
        grabClip(true);
        wakeCount = wakeCount + 1;
        printf("wake word \"Pixel\" detected! (%.2f)\n", kws::lastProb());
      }
      static float prevF[kws::BANDS];
      const float* f = kws::lastFrame();
      float flux = 0;
      for (int b = 0; b < kws::BANDS; ++b) {
        flux += fmaxf(0.f, f[b] - prevF[b]);
        prevF[b] = f[b];
      }
      beat.feed(flux, loudAhead || spkLevel > 0.f || esp_timer_get_time() - lastLoudUs < 500000);
    } else {
      kwsWasOn = false;
      beat.feed(0.f, true);
    }
    beatConf = beat.conf;
    beatBpm = beat.bpm();
    beatPos = beat.position();
    beatDrive = beat.drive();
    beatLoud = beatLoud + (fminf(1.f, fmaxf(0.f, (db - noise - 3.f) / 18.f)) - beatLoud) * 0.02f;  // ~1 s
    if (upOn) {  // continuous stream; silence while the speaker talks (no echo of the own voice)
      // automatic gain: speech arrives at only ~-40 dBFS, too quiet for the server's voice detection.
      // Up to x4 (+12 dB), reduced quickly when it gets loud so close speech does not clip.
      static int16_t mono[FRAME];
      static float env = 0;
      bool mute = uplinkMuted();
      for (int i = 0; i < n; ++i) {
        float x = buf[2 * i + ch];
        float a = fabsf(x);
        env = a > env ? a : env * 0.99995f;  // fast attack, ~1.3 s release
        float g = env > 1.f ? fminf(4.f, 22000.f / env) : 4.f;
        float y = x * g;
        mono[i] = mute ? 0 : (int16_t)fmaxf(-32767.f, fminf(32767.f, y));
      }
      xStreamBufferSend(up, mono, n * 2, 0);
    }
    st.voice = voice;
    st.level = level;
    st.pitch = pitch;
    st.dbfs = db;
    st.noise = noise;
    if (debug && ++frames % 10 == 0)
      printf("mic %6.1f dB  floor %6.1f  snr %5.1f  band %.2f  zcr %.3f  pixel %.2f  %s\n", db, noise, snr, ratio,
             zcr, kws::lastProb(), voice ? "VOICE" : "");
  }
}

// ---- speaker
// The live session streams audio continuously (silence between sentences), paced in real
// time. A jitter buffer absorbs network bursts: playback starts once PREBUFFER is queued
// and re-buffers briefly after an underrun. The amplifier is switched by looking ahead in
// the queue, so it is on ~100 ms before the first syllable (nothing clipped) and off in
// longer pauses (no hiss). If the queue creeps up (clock drift) silent samples are skipped.
static const int RING = 16000 * 20;
static int16_t* ring = nullptr;
static volatile uint32_t ringW = 0, ringR = 0;
static volatile int64_t lastPushUs = 0;  // last time the network delivered audio
static const int LOUD = 260;            // |sample| above this counts as audible speech
static volatile bool continuous = true;  // GPT-Live streams in real time; Realtime sends bursts ahead
static volatile uint32_t toneFrom = 0, toneTo = 0;  // ring range holding a tone (not speech)
static inline bool isTone(uint32_t i) { return i - toneFrom < toneTo - toneFrom; }
static volatile int64_t lastToneUs = 0;  // the amplifier stays on a little after a tone (DMA still playing)

static void playerTask(void*) {
  static int16_t st[2 * 256];
  const uint32_t PREBUFFER = 16000 * 22 / 100;  // 220 ms
  const uint32_t REBUFFER = 16000 * 12 / 100;   // 120 ms after an underrun
  const uint32_t TOO_MUCH = 16000 * 7 / 10;      // more than 700 ms queued: catch up in silence
  const uint32_t AHEAD = 16000 * 15 / 100;       // look 150 ms ahead for speech
  const int64_t AMP_HOLD = 1500000;              // keep the amp on through pauses up to 1.5 s
  size_t w;
  bool paOn = false, running = false;
  uint32_t need = PREBUFFER;
  for (;;) {
    int64_t now = esp_timer_get_time();
    uint32_t avail = ringW - ringR;
    // look ahead: speech coming up?
    bool ahead = false;
    bool toneAhead = false;
    for (uint32_t i = 0, n = avail < AHEAD ? avail : AHEAD; i < n && !ahead; i += 4) {
      bool l = abs(ring[(ringR + i) % RING]) > LOUD;
      if (l && isTone(ringR + i))
        toneAhead = true;
      else
        ahead |= l;
    }
    loudAhead = ahead;
    ahead |= toneAhead;
    // during a conversation the amplifier stays on: switched on by the look-ahead it sometimes missed
    // a soft first sound ("Hallo", "So ...") and its start-up cut the first letter
    if ((ahead || upOn) && !paOn) {
      gpio_set_level(PIN_PA, 1);  // amplifier start-up before the first syllable
      paOn = true;
    } else if (!ahead && !upOn && paOn && now - lastLoudUs > AMP_HOLD && now - lastToneUs > 300000) {
      gpio_set_level(PIN_PA, 0);
      paOn = false;
    }
    bool streaming = now - lastPushUs < 300000;
    if (!streaming && !avail) need = PREBUFFER;  // stream ended: a new one starts with the full buffer
    if (isTone(ringR)) need = 0;  // a tone plays right away (no jitter buffer needed)
    if (!running) {
      if (avail < need && (streaming || !avail)) {  // still filling the jitter buffer
        spkLevel = 0;
        vTaskDelay(pdMS_TO_TICKS(5));
        continue;
      }
      running = true;
    }
    if (!avail) {  // underrun: the I2S driver outputs silence, collect a little again
      running = false;
      need = REBUFFER;
      spkLevel = 0;
      continue;
    }
    {
      int m = avail < 256 ? avail : 256;
      float e = 0;
      bool loud = false;
      bool tone = isTone(ringR);
      for (int i = 0; i < m; ++i) {
        int16_t v = ring[(ringR + i) % RING];
        st[2 * i] = st[2 * i + 1] = v;
        e += (float)v * v;
        loud |= abs(v) > LOUD && !tone;
      }
      ringR += m;
      if (!loud && !tone && avail > TOO_MUCH && continuous) continue;  // clock drift: drop a silent chunk
      spkLevel = tone ? 0.f : fminf(1.f, sqrtf(e / m) / 6000.f);
      if (loud) lastLoudUs = esp_timer_get_time();
      if (tone) lastToneUs = esp_timer_get_time();
      i2s_channel_write(tx, st, m * 4, &w, portMAX_DELAY);  // blocks: paced by the I2S clock
    }
  }
}

// ---- uplink: microphone PCM for the live session

bool start(const char* wakeModel) {
  ring = (int16_t*)heap_caps_malloc(RING * 2, MALLOC_CAP_SPIRAM);
  // 6 s: everything said after the wake word is kept while the session starts
  up = xStreamBufferCreateWithCaps(6 * 32000, 1, MALLOC_CAP_SPIRAM);
  preRing = (int16_t*)heap_caps_calloc(PRE, 2, MALLOC_CAP_SPIRAM);
  clip = (int16_t*)heap_caps_calloc(CLIP, 2, MALLOC_CAP_SPIRAM);
  if (!i2sInit()) {
    ESP_LOGE(TAG, "i2s init failed");
    return false;
  }
  if (!codecInit()) {
    ESP_LOGE(TAG, "ES8311 not answering");
    return false;
  }
  ESP_LOGI(TAG, "ES8311 ready, chip id %02x %02x", rd(0xFD), rd(0xFE));
  wakeInit(wakeModel);  // reads the model from flash: must run on an internal-RAM stack
  kws::init();
  // stack in PSRAM: internal RAM is needed for WiFi / TLS
  xTaskCreatePinnedToCoreWithCaps(task, "mic", 12288, nullptr, 6, nullptr, 0, MALLOC_CAP_SPIRAM);
  xTaskCreatePinnedToCore(playerTask, "speaker", 4096, nullptr, 7, nullptr, 0);
  return true;
}

State get() {
  State s;
  s.voice = st.voice;
  s.level = st.level;
  s.pitch = st.pitch;
  s.dbfs = st.dbfs;
  s.noise = st.noise;
  return s;
}
void setDebug(bool on) { debug = on; }
void music(float& conf, float& bpm, float& pos, float& loud, float& drive) {
  drive = beatDrive;
  conf = beatConf;
  bpm = beatBpm;
  pos = beatPos;
  loud = beatLoud;
}
int wakeWords() { return wakeCount; }
void setWake(bool on) { wakeOn = on; }
void fakeWake() { fakeWakeReq = true; }
static void grabClip(bool pixel) {  // runs in the mic task right after a detection; it appends the rest
  clipPixel = pixel;
  uint32_t w = preW;
  for (int i = 0; i < PRE; ++i) clip[i] = preRing[(w + i) % PRE];
  clipN = PRE;
  clipPost = CLIP_POST;
}
bool wakeClipPixel() { return clipPixel; }
int wakeClip(int16_t* dst, int cap) {
  int n = clipN < cap ? clipN : cap;
  memcpy(dst, clip, n * 2);
  return n;
}
int record(int16_t* dst, int samples) {
  // pre-roll: the half second before the request
  uint32_t w = preW;
  const int HALF = RATE / 2;
  int pre = samples > HALF ? HALF : 0;
  for (int i = 0; i < pre; ++i) dst[i] = preRing[(w + PRE - HALF + i) % PRE];
  dst += pre;
  samples -= pre;
  capGot = 0;
  capN = samples;
  capBuf = dst;
  int64_t end = esp_timer_get_time() + (int64_t)samples * 1000000 / RATE + 500000;
  while (capGot < samples && esp_timer_get_time() < end) vTaskDelay(pdMS_TO_TICKS(20));
  capBuf = nullptr;
  return capGot + pre;
}
void setWakeWords(bool pixel, bool halloEsp, float sensitivity) {
  usePixel = pixel;
  useEsp = halloEsp;
  kws::setSensitivity(sensitivity);
}
void wakeStop() {
#if CFG_WAKEWORD_ENABLE
  wnStopped = true;
  vTaskDelay(pdMS_TO_TICKS(200));  // a detection running right now is through
#endif
}
bool hasWakeModel(const char* model) {
#if CFG_WAKEWORD_ENABLE
  for (int i = 0; models && i < models->num; ++i)
    if (!strcmp(models->model_name[i], model)) return true;
#endif
  return false;
}
bool audible() { return loudAhead || esp_timer_get_time() - lastLoudUs < 250000; }
void streamPush(const int16_t* pcm, int n) {
  if (!ring) return;
  int room = RING - (int)(ringW - ringR);
  if (n > room) n = room;
  for (int i = 0; i < n; ++i) ring[(ringW + i) % RING] = pcm[i];
  ringW += n;
  lastPushUs = esp_timer_get_time();
}
void streamFlush() {
  // an interruption drops the speech, but not the wake word tone that is still to be played
  uint32_t pend = toneTo - ringR;
  if (pend && pend <= ringW - ringR && toneTo - toneFrom < (uint32_t)RATE)
    ringW = toneTo;
  else
    ringR = ringW;
}
int streamQueued() { return (int)(ringW - ringR); }
void beep(int hz, int ms, float level) {
  if (!ring) return;
  int n = RATE * ms / 1000, room = RING - (int)(ringW - ringR);
  if (n > room) return;
  uint32_t w = ringW;
  float a = 12000.f * level;
  for (int i = 0; i < n; ++i) {
    float env = fminf(1.f, fminf(i, n - i) / (RATE * 0.006f));  // 6 ms fades: no clicks
    ring[(w + i) % RING] = (int16_t)(a * env * sinf(6.2831853f * hz * i / RATE));
  }
  bool pending = toneTo != ringR && toneTo - ringR <= w - ringR;  // the last tone is still queued
  if (w != toneTo || !pending) toneFrom = w;  // tones back to back (a little melody) form one range
  toneTo = w + n;
  ringW = w + n;
  lastPushUs = esp_timer_get_time();
}
void setStreamContinuous(bool on) { continuous = on; }
void setVolume(int percent) {
  if (!codec) return;
  if (percent < 0) percent = 0;
  if (percent > 100) percent = 100;
  wr(0x32, (uint8_t)(0xBF - (100 - percent) * 0.8f));
}
void setUplink(bool on) {
  if (on && !upOn && up) xStreamBufferReset(up);
  upOn = on;
}
int uplinkRead(int16_t* dst, int maxSamples, int timeoutMs) {
  return up ? xStreamBufferReceive(up, dst, maxSamples * 2, pdMS_TO_TICKS(timeoutMs)) / 2 : 0;
}
// mute the uplink while speech is audible, plus a short echo tail
bool uplinkMuted() { return esp_timer_get_time() - lastLoudUs <= 300000; }
float speakLevel() { return spkLevel; }

}  // namespace mic
