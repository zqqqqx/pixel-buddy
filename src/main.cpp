// Animated eyes firmware for the Spotpear ESP32-S3 1.54" "muma" board
// (ST7789 240x240 over SPI, CST816D touch, BOOT button).
//
// Pipeline per frame:
//   frame task (core 1): read touch/button/serial -> eyes::update() -> dirty rect -> split in bands
//   both cores render bands into DMA buffers (work stealing via an atomic counter)
//   sender task pushes finished bands to the panel; the DMA-done ISR recycles the buffers
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <atomic>
#include <initializer_list>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "driver/usb_serial_jtag.h"
#include "esp_heap_caps.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "config.h"
#include "cJSON.h"
#include "esp_attr.h"
#include "eyes.h"
#include "mic.h"
#include "live.h"
#include "net.h"
#include "sd.h"
#include "settings.h"
#include "web.h"
#include "assist.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_sleep.h"
#include "ha.h"
#include "leds.h"
#include "screen.h"
#include "battery.h"
#include <time.h>

static const char* TAG = "eyes";

// pins: xiaozhi-esp32 main/boards/spotpear/sp-esp32-s3-1.54-muma/config.h
#define PIN_LCD_SCLK GPIO_NUM_4
#define PIN_LCD_MOSI GPIO_NUM_2
#define PIN_LCD_CS GPIO_NUM_5
#define PIN_LCD_DC GPIO_NUM_47
#define PIN_LCD_RST GPIO_NUM_38
#define PIN_LCD_BL GPIO_NUM_42  // active low
#define PIN_TP_SDA GPIO_NUM_11
#define PIN_TP_SCL GPIO_NUM_7
#define PIN_TP_RST GPIO_NUM_6
#define PIN_BOOT GPIO_NUM_0
#define PIN_POWER_HOLD GPIO_NUM_3
#define PIN_AUDIO_PA GPIO_NUM_46
#define LCD_PCLK_HZ (80 * 1000 * 1000)  // the ST7789 takes it (stock firmware: 40 MHz)

static const int BAND_PIX = 240 * 20;
static const int NBUF = 5;
static int64_t frameUs = 1000000 / CFG_MAX_FPS;  // from the settings
static float brightness = CFG_BRIGHTNESS;  // wanted (setting, night, schedule)
static float backlight = 0;                 // what the backlight shows right now
static bool displayDark = false;            // display off timeout: nobody there

// ============================================================== display
static esp_lcd_panel_handle_t panel;
static QueueHandle_t freeQ, sendQ, flightQ;
struct Job {
  uint16_t* buf;
  int16_t x0, y0, x1, y1;
};

static bool IRAM_ATTR onColorDone(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t*, void*) {
  BaseType_t woken = pdFALSE;
  uint16_t* buf;
  if (xQueueReceiveFromISR(flightQ, &buf, &woken) == pdTRUE) xQueueSendFromISR(freeQ, &buf, &woken);
  return woken == pdTRUE;
}

static void senderTask(void*) {
  Job j;
  for (;;) {
    xQueueReceive(sendQ, &j, portMAX_DELAY);
    xQueueSend(flightQ, &j.buf, portMAX_DELAY);
    esp_lcd_panel_draw_bitmap(panel, j.x0, j.y0, j.x1, j.y1, j.buf);
  }
}

static void backlightInit() {
  ledc_timer_config_t t = {};
  t.speed_mode = LEDC_LOW_SPEED_MODE;
  t.duty_resolution = LEDC_TIMER_10_BIT;
  t.timer_num = LEDC_TIMER_0;
  t.freq_hz = 25000;
  t.clk_cfg = LEDC_AUTO_CLK;
  ESP_ERROR_CHECK(ledc_timer_config(&t));
  ledc_channel_config_t c = {};
  c.gpio_num = PIN_LCD_BL;
  c.speed_mode = LEDC_LOW_SPEED_MODE;
  c.channel = LEDC_CHANNEL_0;
  c.timer_sel = LEDC_TIMER_0;
  c.duty = 0;
  c.flags.output_invert = 1;
  ESP_ERROR_CHECK(ledc_channel_config(&c));
}
static void backlightSet(float v) {
  if (v < 0) v = 0;
  if (v > 1) v = 1;
  static uint32_t last = UINT32_MAX;
  uint32_t duty = (uint32_t)(v * 1023.f);
  if (duty == last) return;
  last = duty;
  ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
  ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static void lcdInit() {
  spi_bus_config_t bus = {};
  bus.mosi_io_num = PIN_LCD_MOSI;
  bus.miso_io_num = GPIO_NUM_NC;
  bus.sclk_io_num = PIN_LCD_SCLK;
  bus.quadwp_io_num = GPIO_NUM_NC;
  bus.quadhd_io_num = GPIO_NUM_NC;
  bus.max_transfer_sz = BAND_PIX * 2 + 64;
  ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO));

  esp_lcd_panel_io_handle_t io = nullptr;
  esp_lcd_panel_io_spi_config_t ioc = {};
  ioc.cs_gpio_num = PIN_LCD_CS;
  ioc.dc_gpio_num = PIN_LCD_DC;
  ioc.spi_mode = 0;
  ioc.pclk_hz = LCD_PCLK_HZ;
  ioc.trans_queue_depth = 10;
  ioc.on_color_trans_done = onColorDone;
  ioc.lcd_cmd_bits = 8;
  ioc.lcd_param_bits = 8;
  ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI3_HOST, &ioc, &io));

  esp_lcd_panel_dev_config_t pc = {};
  pc.reset_gpio_num = PIN_LCD_RST;
  pc.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
  pc.bits_per_pixel = 16;
  ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &pc, &panel));
  ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
  ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
  ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, false));
  ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, false, false));
  ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, true));
  uint8_t vcom = 0x38;  // same VCOM tweak as the stock firmware
  esp_lcd_panel_io_tx_param(io, 0xBB, &vcom, 1);
  ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));
}

// ============================================================== band rendering on both cores
struct FrameJob {
  int x0, y0, x1, y1, rows, nb, mode;  // mode: 0 eyes, 1 setup screen, 2 black
  uint32_t gen;
};
// A frame is cut into bands. The frame task (core 1) takes them from the top, the helper on core 0 from
// the bottom. Core 0 also runs the microphone, WiFi and TLS, so a band it holds can be 10-100 ms late:
// core 1 never waits long for it - it renders that band itself and the helper's copy is dropped. Core 0
// only adds its spare time. Only the frame task sends to the display, so the bands arrive in order.
enum { BAND_FREE, BAND_HELPER, BAND_MINE, BAND_READY };  // low 2 bits; above them the frame number
static const int MAXB = 32;
static std::atomic<uint32_t> bandSt[MAXB];
static uint16_t* bandBuf[MAXB];       // the helper's finished band (BAND_READY)
static int64_t bandT0[MAXB];          // when the helper started it
static QueueHandle_t helperQ;         // the current FrameJob for the helper (one slot, overwritten)
static TaskHandle_t frameTaskH, workerTaskH;
static volatile bool clearMode = false;
static std::atomic<int64_t> statRender{0}, statWait{0};
static std::atomic<int> statHelped{0}, statRescued{0};

static uint16_t* renderBand(const FrameJob& j, int k) {
  int y0 = j.y0 + k * j.rows, y1 = y0 + j.rows < j.y1 ? y0 + j.rows : j.y1;
  uint16_t* buf;
  int64_t t0 = esp_timer_get_time();
  xQueueReceive(freeQ, &buf, portMAX_DELAY);
  int64_t t1 = esp_timer_get_time();
  if (j.mode == 2)
    memset(buf, 0, (size_t)(j.x1 - j.x0) * (y1 - y0) * 2);
  else if (j.mode == 1)
    screen::render(buf, j.x0, y0, j.x1, y1);
  else
    eyes::render(buf, j.x0, y0, j.x1, y1);
  statWait += t1 - t0;
  statRender += esp_timer_get_time() - t1;
  return buf;
}
static void sendBand(const FrameJob& j, int k, uint16_t* buf) {
  int y0 = j.y0 + k * j.rows, y1 = y0 + j.rows < j.y1 ? y0 + j.rows : j.y1;
  Job jb = {buf, (int16_t)j.x0, (int16_t)y0, (int16_t)j.x1, (int16_t)y1};
  xQueueSend(sendQ, &jb, portMAX_DELAY);
}

static void workerTask(void*) {
  FrameJob j;
  for (;;) {
    xQueueReceive(helperQ, &j, portMAX_DELAY);
    const uint32_t g = j.gen << 2;
    for (int k = j.nb - 1; k >= 0; --k) {
      uint32_t want = g | BAND_FREE;
      bandT0[k] = esp_timer_get_time();  // before the claim: core 1 reads it once it sees BAND_HELPER
      if (!bandSt[k].compare_exchange_strong(want, g | BAND_HELPER)) break;  // core 1 got this far
      uint16_t* buf = renderBand(j, k);
      bandBuf[k] = buf;
      want = g | BAND_HELPER;
      if (!bandSt[k].compare_exchange_strong(want, g | BAND_READY)) {  // core 1 did it meanwhile
        xQueueSend(freeQ, &buf, 0);
        break;
      }
      statHelped++;
    }
  }
}

static void drawRect(const eyes::Rect& r) {
  static uint32_t gen = 0;
  static int64_t bandUs = 3000;  // how long a band takes on core 1 (running average)
  int w = r.x1 - r.x0;
  int rows = BAND_PIX / w;
  if (rows > 60) rows = 60;
  int nb = (r.y1 - r.y0 + rows - 1) / rows;
  if (nb > MAXB) {  // never with the band size above; a taller band instead of overflowing
    rows = (r.y1 - r.y0 + MAXB - 1) / MAXB;
    nb = (r.y1 - r.y0 + rows - 1) / rows;
  }
  gen = (gen + 1) & 0x3FFFFFFF;
  const uint32_t g = gen << 2;
  FrameJob j = {r.x0, r.y0, r.x1, r.y1, rows, nb, clearMode ? 2 : screen::active() ? 1 : 0, gen};
  for (int k = 0; k < nb; ++k) bandSt[k].store(g | BAND_FREE);
  xQueueOverwrite(helperQ, &j);
  // the helper's finished bands go out right away: they hold buffers the frame task needs
  auto collect = [&]() {
    for (int k = nb - 1; k >= 0; --k) {
      uint32_t want = g | BAND_READY;
      if (bandSt[k].compare_exchange_strong(want, g | BAND_MINE)) sendBand(j, k, bandBuf[k]);
    }
  };
  for (int k = 0; k < nb; ++k) {  // own share, from the top
    uint32_t want = g | BAND_FREE;
    if (!bandSt[k].compare_exchange_strong(want, g | BAND_MINE)) continue;
    int64_t t = esp_timer_get_time();
    sendBand(j, k, renderBand(j, k));
    bandUs += (esp_timer_get_time() - t - bandUs) / 8;
    collect();
  }
  for (int k = 0; k < nb; ++k) {  // the helper's share: collect it, or take over a band that is late
    for (;;) {
      uint32_t st = bandSt[k].load();
      if ((st & 3) == BAND_READY) {
        collect();
        break;
      }
      if ((st & 3) != BAND_HELPER) break;  // (never: every band is claimed by now)
      // a band the helper started a while ago and still has: it was pushed aside on core 0
      if (esp_timer_get_time() - bandT0[k] > bandUs + bandUs / 4) {
        uint32_t want = g | BAND_HELPER;
        if (bandSt[k].compare_exchange_strong(want, g | BAND_MINE)) {
          sendBand(j, k, renderBand(j, k));
          statRescued++;
          break;
        }
        continue;  // it just finished
      }
      esp_rom_delay_us(20);
    }
  }
}

// ============================================================== touch (CST816D @0x15)
static i2c_master_dev_handle_t tp = nullptr;
static bool tpDebug = true;

static void touchInit() {
  gpio_config_t g = {};
  g.pin_bit_mask = 1ULL << PIN_TP_RST;
  g.mode = GPIO_MODE_OUTPUT;
  gpio_config(&g);
  gpio_set_level(PIN_TP_RST, 0);
  vTaskDelay(pdMS_TO_TICKS(10));
  gpio_set_level(PIN_TP_RST, 1);
  vTaskDelay(pdMS_TO_TICKS(60));

  i2c_master_bus_handle_t bus;
  i2c_master_bus_config_t bc = {};
  bc.i2c_port = I2C_NUM_1;
  bc.sda_io_num = PIN_TP_SDA;
  bc.scl_io_num = PIN_TP_SCL;
  bc.clk_source = I2C_CLK_SRC_DEFAULT;
  bc.glitch_ignore_cnt = 7;
  bc.flags.enable_internal_pullup = 1;
  if (i2c_new_master_bus(&bc, &bus) != ESP_OK) return;
  i2c_device_config_t dc = {};
  dc.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  dc.device_address = 0x15;
  dc.scl_speed_hz = 400000;
  if (i2c_master_bus_add_device(bus, &dc, &tp) != ESP_OK) {
    tp = nullptr;
    return;
  }
  esp_log_level_set("i2c.master", ESP_LOG_NONE);
  uint8_t noSleep[2] = {0xFE, 0x01};  // DisAutoSleep
  esp_err_t e = i2c_master_transmit(tp, noSleep, 2, 30);
  ESP_LOGI(TAG, "touch init: %s", esp_err_to_name(e));
}

static bool touchRead(int& x, int& y) {
  if (!tp) return false;
  uint8_t reg = 0x02, d[5];
  if (i2c_master_transmit_receive(tp, &reg, 1, d, 5, 15) != ESP_OK) return false;
  if ((d[0] & 0x0F) == 0) return false;
  x = ((d[1] & 0x0F) << 8) | d[2];
  y = ((d[3] & 0x0F) << 8) | d[4];
  return x < 260 && y < 260;
}

// ============================================================== conversation
// wake word -> GPT-Live session. The session posts eye events (reactions picked live from
// what is said, listening moods, errors, "busy" while the backend works).
static bool eyesListening = false;

static bool night = false;     // night mode right now
static bool privacy = false;   // "hoer nicht mehr zu": no wake word until touched
static uint32_t lampRgb = 0;   // a lamp just changed: its colour on the LEDs until lampUntil
static int64_t lampUntil = 0;
static bool wakeBeep = true;

// someone is there (touch, button, wake word, web, conversation): keeps the display on (display off timeout)
static int64_t lastActive = 0;
static void activity() { lastActive = esp_timer_get_time(); }

static void ringStop(bool touched);
static void startLive(bool byWakeWord = true) {
  activity();
  ringStop(false);  // "Pixel" while it rings: that is the answer to it
  if (live::active()) return;
  printf("%s -> live session\n", byWakeWord ? "wake word" : "request");
  if (byWakeWord && wakeBeep) {  // "I heard you": two short rising tones
    mic::beep(880, 70, night ? 0.12f : 0.3f);
    mic::beep(1320, 90, night ? 0.12f : 0.3f);
  }
  eyes::wakeWord();
  eyesListening = true;
  mic::setWake(false);
  live::begin(byWakeWord);
}

// ---- timers / alarms: a ringing alarm clock until touched (at most a minute), then the reminder
static int64_t ringUntil = 0, ringNext = 0;
static char ringLabel[48];
static void ringStop(bool touched) {
  if (!ringUntil) return;
  ringUntil = 0;
  mic::streamFlush();  // the rings still queued would play into what comes next
  if (touched && ringLabel[0]) {  // say what it was for
    char t[160];
    snprintf(t, sizeof(t), "(Hinweis vom System: dein Timer \"%s\" ist gerade abgelaufen - sag mir das in einem "
             "ganz kurzen Satz.)", ringLabel);
    live::say(t);
    startLive(false);
  }
}
static void ringTick(int64_t now) {
  char label[48];
  if (!ringUntil && !live::active() && assist::popDue(label, sizeof(label))) {
    printf("timer \"%s\" is due\n", label);
    snprintf(ringLabel, sizeof(ringLabel), "%s", label);
    ringUntil = now + 60000000;
    ringNext = now;
  }
  if (!ringUntil) return;
  if (now > ringUntil || live::active()) {
    ringStop(false);
    return;
  }
  if (now >= ringNext) {
    ringNext = now + 3600000;
    eyes::timer(true);
    for (int k = 0; k < 3; ++k) {  // three little double rings
      mic::beep(1568, 90, night ? 0.25f : 0.6f);
      mic::beep(1976, 90, night ? 0.25f : 0.6f);
      mic::beep(1, 300, 0.f);
    }
  }
}

// ---- off: display dark, speaker off, power released (battery); on USB deep sleep until BOOT is pressed
static void shutdown() {
  printf("shutting down\n");
  backlightSet(0);
  esp_lcd_panel_disp_on_off(panel, false);
  esp_lcd_panel_disp_sleep(panel, true);
  gpio_set_level(PIN_AUDIO_PA, 0);
  vTaskDelay(pdMS_TO_TICKS(200));
  gpio_set_level(PIN_POWER_HOLD, 0);  // on battery this switches the board off
  vTaskDelay(pdMS_TO_TICKS(500));
  ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 1);  // backlight is active low: high = dark
  for (gpio_num_t p : {PIN_LCD_BL, PIN_AUDIO_PA}) gpio_hold_en(p);
  gpio_deep_sleep_hold_en();
  esp_sleep_enable_ext0_wakeup(PIN_BOOT, 0);
  esp_deep_sleep_start();
}

static void runLive() {
  static live::Phase last = live::IDLE;
  static bool talking = false;
  static live::Event pending;  // a reaction that waits until the current one is through
  static int64_t pendingUs = 0;
  static int64_t holdUntil = 0;  // an animation asked for by voice plays undisturbed
  live::Phase p = live::phase();
  int64_t now = esp_timer_get_time();
  live::Event ev;
  while (live::popEvent(ev)) {
    switch (ev.kind) {
      case live::EV_REACT:
        if (now < holdUntil) break;
        pending = ev;
        pendingUs = now;
        break;
      case live::EV_PLAY: {
        printf("eyes: playing \"%s\" on request\n", ev.name);
        pendingUs = 0;
        eyesListening = false;
        holdUntil = now + 4500000;
        bool err = false;
        for (const char* e : {"offline", "quota", "cant", "huh", "locked", "glitch"}) err |= !strcmp(e, ev.name);
        int seq[6], sn = settings::custom(ev.name, seq);  // an own animation (web interface -> Animationen)
        if (sn) {
          eyes::sequence(seq, sn);
          holdUntil = now + 2500000LL * sn;
        } else if (err) {
          eyes::error(ev.name);
        } else if (!strncmp(ev.name, "light_", 6)) {
          eyes::home(ev.name, 0xFFC46E);
        } else if (!strcmp(ev.name, "timer")) {
          eyes::timer(false);
        } else if (!strncmp(ev.name, "look_", 5)) {  // look_left / right / up / down
          const char* d = ev.name + 5;
          eyes::lookAt(!strcmp(d, "left") ? -1.f : !strcmp(d, "right") ? 1.f : 0.f,
                       !strcmp(d, "up") ? -0.9f : !strcmp(d, "down") ? 0.9f : 0.f, 4.f);
        } else {
          const char* a = ev.name;
          eyes::react(&a, 1);
        }
        break;
      }
      case live::EV_MOOD:
        eyes::listenMood(ev.name);
        break;
      case live::EV_HOME:
        printf("eyes: smart home \"%s\" #%06lx\n", ev.name, (unsigned long)ev.rgb);
        pendingUs = 0;
        eyesListening = false;
        holdUntil = now + 3000000;
        eyes::home(ev.name, ev.rgb);
        lampRgb = strcmp(ev.name, "light_off") ? ev.rgb : 0;
        lampUntil = now + 3000000;
        break;
      case live::EV_WEATHER:
        printf("eyes: weather \"%s\"\n", ev.name);
        pendingUs = 0;
        eyesListening = false;
        holdUntil = now + 4000000;
        eyes::weather(ev.name);
        break;
      case live::EV_SYS:
        printf("self management: %s\n", ev.name);
        if (!strcmp(ev.name, "shutdown")) shutdown();
        if (!strcmp(ev.name, "sleep")) eyes::trigger(eyes::behaviorIndex("falling asleep"));
        if (!strcmp(ev.name, "ringstop")) ringStop(false);
        if (!strcmp(ev.name, "privacy")) {
          privacy = true;
          mic::setWake(false);
          eyes::trigger(eyes::behaviorIndex("sleepy"));
        }
        break;
      case live::EV_ERROR:
        if (now < holdUntil) break;
        printf("eyes: error animation \"%s\"\n", ev.name);
        pendingUs = 0;
        eyesListening = false;
        eyes::error(ev.name);
        break;
      case live::EV_BUSY:
        if (now < holdUntil && ev.name[0] == '1') break;
        eyes::busy(ev.name[0] == '1');
        if (ev.name[0] == '1') eyesListening = false;
        break;
    }
  }
  if (pendingUs && now - pendingUs > 2500000) pendingUs = 0;  // too late, the moment has passed
  if (pendingUs && !eyes::reacting()) {
    const char* a = pending.name;
    eyes::react(&a, 1);
    eyesListening = false;
    pendingUs = 0;
  }
  if (p == live::USER_TALKING && !eyesListening && !eyes::reacting()) {  // follow-up: perk up again
    eyes::wakeWord();
    eyesListening = true;
  }
  if (p == live::RESPONDING && eyesListening) {  // the answer starts: stop listening
    eyes::react(nullptr, 0);
    eyesListening = false;
  }
  if (mic::audible()) {
    eyes::speaking(mic::speakLevel());
    talking = true;
  } else if (talking) {
    eyes::speaking(-1.f);
    talking = false;
  }
  if (p == live::IDLE && last != live::IDLE) {
    mic::setWake(!privacy);
    if (eyesListening) eyes::react(nullptr, 0);  // nobody spoke: just stop listening
    eyesListening = false;
  }
  last = p;
}

// ---- BOOT pressed 5x quickly: a menu on the display (restart, switch off, factory settings)
static bool menuOpen = false;
static int menuStage = 0;  // 0 main, 1 "really reset?"
static int64_t menuAt = 0;
static void menuShow(int stage) {
  menuStage = stage;
  menuAt = esp_timer_get_time();
  if (stage == 0) {
    static const char* const M[] = {"Neustart", "Ausschalten", "Werkseinstellung", "Zurueck"};
    screen::menu("Pixel", M, 4, 2);
  } else {
    static const char* const M[] = {"Ja, loeschen", "Abbrechen"};
    screen::menu("Wirklich?", M, 2, 0);
  }
  menuOpen = true;
}
static void menuClose() {
  menuOpen = false;
  screen::hide();
}
static void factoryReset() {
  printf("factory reset\n");
  settings::factory();  // look, personality, smart home, OpenAI key
  assist::wipe();       // timers, memory, costs
  net::forgetWifi();    // the network from the setup (a network built into the firmware stays)
  remove("/sdcard/pixel/chat.log");
  remove("/sdcard/pixel/chat.old.log");
  vTaskDelay(pdMS_TO_TICKS(300));
  esp_restart();
}
static void menuTap(int x, int y) {
  menuAt = esp_timer_get_time();
  int i = screen::hit(x, y);
  if (menuStage == 0) {
    if (i == 0) esp_restart();
    if (i == 1) shutdown();
    if (i == 2) menuShow(1);
    if (i == 3) menuClose();
  } else {
    if (i == 0) factoryReset();
    if (i == 1) menuShow(0);
  }
}

// ============================================================== inputs
static void pollInputs() {
  static bool wasDown = false;
  int x = 0, y = 0;
  bool down = touchRead(x, y);
  if (down) activity();
  if (down && tpDebug && !wasDown) printf("touch down x=%d y=%d\n", x, y);
  if (down && !wasDown && menuOpen) {
    menuTap(x, y);
    wasDown = down;
    return;
  }
  if (down && !wasDown) {
    if (screen::active()) net::stopSetup();  // setup page shown although online: a tap closes it
    if (live::active()) live::interrupt();  // touched while it talks: stop
    ringStop(true);
    if (privacy) {
      privacy = false;
      if (!live::active()) mic::setWake(true);
    }
  }
  wasDown = down;
  eyes::touch(down, x, y);

  static bool wasVoice = false;
  mic::State m = mic::get();
  eyes::voice(m.voice, m.level, m.pitch);
  float mc, mb, mp, ml, md;  // music nearby: the eyes dance along (speech / TV talk stay below ~0.45)
  mic::music(mc, mb, mp, ml, md);
  eyes::music(fminf(1.f, fmaxf(0.f, (mc - 0.55f) * 4.f)), mb, mp, ml, md, !night && !privacy);
  static int wakes = 0;
  int wk = mic::wakeWords();
  if (wk != wakes) {
    wakes = wk;
    startLive();
  }
  runLive();
  if (m.voice != wasVoice) printf(m.voice ? "voice: listening\n" : "voice: quiet\n");
  wasVoice = m.voice;

  static int64_t pressT = 0;
  static bool pressed = false, longFired = false;
  static int clicks = 0;
  static int64_t lastClick = 0;
  bool b = gpio_get_level(PIN_BOOT) == 0;
  int64_t now = esp_timer_get_time();
  if (b) activity();
  if (b && !pressed) {
    pressed = true;
    longFired = false;
    pressT = now;
    ringStop(true);
  } else if (b && pressed && !longFired && now - pressT > 700000) {
    longFired = true;
    eyes::buttonLong();
  } else if (!b && pressed) {
    pressed = false;
    if (!longFired && now - pressT > 30000) {
      clicks = now - lastClick < 600000 ? clicks + 1 : 1;
      lastClick = now;
      if (clicks >= 5) {
        clicks = 0;
        printf("BOOT 5x: menu\n");
        menuShow(0);
      } else if (!menuOpen) {
        eyes::buttonClick();
      }
    }
  }

  // serial: digits/letters trigger behaviours, 'c' colour, 'n' next, 'h' help
  uint8_t ch[8];
  int n = usb_serial_jtag_read_bytes(ch, sizeof(ch), 0);
  for (int i = 0; i < n; ++i) {
    char c = (char)ch[i];
    if (c == 'h') {
      for (int k = 0; k < eyes::behaviorCount(); ++k)
        printf("  %c = %s\n", k < 10 ? '0' + k : 'A' + k - 10, eyes::behaviorName(k));
      printf("  n = next, c = next colour, t = toggle touch log, m = toggle mic log, b = benchmark\n"
             "  w = as if \"Pixel\" was heard (incl. the recording), p = CPU time per task since the last 'p'\n"
             "  ! = cycle through the error animations\n");
    } else if (c >= '0' && c <= '9') {
      eyes::trigger(c - '0');
    } else if (c >= 'A' && c <= 'Z') {
      eyes::trigger(10 + c - 'A');
    } else if (c == 'w') {
      mic::fakeWake();
    } else if (c == '!') {
      static int k = 0;
      const char* E[] = {"offline", "quota", "cant", "huh", "locked", "glitch"};
      printf("error animation: %s\n", E[k]);
      eyes::error(E[k]);
      k = (k + 1) % 6;
    } else if (c == 'n') {
      eyes::buttonClick();
    } else if (c == 'c') {
      eyes::buttonLong();
    } else if (c == 't') {
      tpDebug = !tpDebug;
    } else if (c == 'm') {
      static bool md = false;
      md = !md;
      mic::setDebug(md);
    } else if (c == 'b') {
      eyes::bench();
    } else if (c == 'p') {  // CPU per task since the last 'p'
      EXT_RAM_BSS_ATTR static char st[2048];
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
      vTaskGetRunTimeStats(st);
#else
      snprintf(st, sizeof(st), "task statistics are off (CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS)\n");
#endif
      printf("%s", st);
    }
  }
}

// ============================================================== settings & status
static uint32_t appliedVersion = 0;
static void applySettings() {
  appliedVersion = settings::version();
  eyes::Look l;
  settings::look(l);
  eyes::setLook(l);
  for (int i = 0; i < eyes::behaviorCount(); ++i) {
    bool on;
    float sp, in;
    if (eyes::behaviorKind(i) != eyes::K_INTERNAL && settings::anim(i, on, sp, in)) eyes::setBehavior(i, on, sp, in);
  }
  frameUs = 1000000 / settings::fps();
  EXT_RAM_BSS_ATTR static settings::Conv c;
  settings::conv(c);
  time_t tnow = time(nullptr);
  struct tm tmv;
  localtime_r(&tnow, &tmv);
  int h = tmv.tm_hour;
  bool clockOk = tnow > 1700000000;
  night = c.nightOn && clockOk &&
          (c.nightFrom > c.nightTo ? (h >= c.nightFrom || h < c.nightTo) : (h >= c.nightFrom && h < c.nightTo));
  int sb = -1, sv = -1;
  bool sched = clockOk && settings::scheduled(h * 60 + tmv.tm_min, sb, sv);
  brightness = (night ? c.nightBright : sched ? sb : settings::brightness()) / 100.f;
  if (night) {  // calmer: fewer idle animations
    l.idleMin *= 2.f;
    l.idleMax *= 2.f;
    eyes::setLook(l);
  }
  wakeBeep = c.wakeBeep;
  mic::setVolume(sched && sv >= 0 ? sv : c.volume);
  mic::setWakeWords(strcmp(c.wake, "halloesp") != 0, strcmp(c.wake, "pixel") != 0, c.wakeSens);
}

static void publishStatus(float fps) {
  static const char* PH[] = {"idle", "connecting", "waiting", "user", "responding"};
  char j[600];
  const esp_app_desc_t* app = esp_app_get_description();
  snprintf(j, sizeof(j),
           "{\"ip\":\"%s\",\"ssid\":\"%s\",\"rssi\":%d,\"uptime\":%lld,\"fps\":%.0f,\"behavior\":\"%s\","
           "\"colorway\":\"%s\",\"live\":\"%s\",\"heap\":%u,\"psram\":%u,\"sd\":%s,\"build\":\"%s %s\","
           "\"usd_today\":%.3f,\"usd_month\":%.3f,\"timers\":%d,\"night\":%s,\"privacy\":%s,\"setup\":%s,"
           "\"ringing\":%s,\"slot\":\"%s\",\"display\":%d,\"battery\":%d,\"charging\":%s}",
           net::ip(), net::ssid(), net::rssi(), esp_timer_get_time() / 1000000, fps, eyes::currentName(),
           eyes::colorwayName(), PH[(int)live::phase()], (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), sd::ok() ? "true" : "false", app->date, app->time,
           assist::today(), assist::month(), assist::timerCount(), night ? "true" : "false",
           privacy ? "true" : "false", net::setupActive() ? "true" : "false", ringUntil ? "true" : "false",
           esp_ota_get_running_partition()->label, (int)(backlight * 100.f + 0.5f), battery::level(),
           battery::charging() ? "true" : "false");
  web::setStatus(j);
}

// ============================================================== the two LEDs
// Status: listening (pulsing in the iris colour), talking (with the voice), thinking (the two take
// turns), alarm (red / white), setup (blue), errors (their colour), lamps (the lamp's colour),
// microphone off (dim red). Mode 2 also glows softly when nothing happens. Dimmed at night.
static uint32_t scale(uint32_t c, float k) {
  k = k < 0 ? 0 : k > 1 ? 1 : k;
  return (uint32_t)((c >> 16 & 255) * k) << 16 | (uint32_t)((c >> 8 & 255) * k) << 8 | (uint32_t)((c & 255) * k);
}
static void ledsTick(int64_t now) {
  int mode, bright;
  settings::leds(mode, bright);
  float t = now * 1e-6f, pulse = 0.55f + 0.45f * sinf(t * 6.2832f);
  uint32_t a = 0, b = 0;
  if (mode > 0) {
    eyes::Look lk;
    settings::look(lk);
    uint32_t iris = lk.colorway >= 0 ? eyes::colorwayColor(lk.colorway, 1) : lk.iris;
    if ((iris >> 16 & 255) + (iris >> 8 & 255) + (iris & 255) < 120)  // dark iris: the accent colour
      iris = lk.colorway >= 0 ? eyes::colorwayColor(lk.colorway, 2) : lk.accent;
    const char* beh = eyes::currentName();
    live::Phase ph = live::phase();
    static const struct {
      const char* name;
      uint32_t rgb;
    } ERR[] = {{"offline", 0xFF7000}, {"quota", 0xFF0000}, {"cant", 0xFFC000}, {"huh", 0xFFC000},
               {"locked", 0xA000FF}, {"glitch", 0xFF0040}};
    uint32_t err = 0;
    for (auto& e : ERR)
      if (!strcmp(beh, e.name)) err = e.rgb;
    if (ringUntil) {
      bool on = (int)(t * 4) & 1;
      a = on ? 0xFF0000 : 0xFFFFFF;
      b = on ? 0xFFFFFF : 0xFF0000;
    } else if (net::setupActive()) {
      a = b = scale(0x0050FF, pulse);
    } else if (err) {
      a = b = scale(err, pulse);
    } else if (now < lampUntil) {
      a = b = lampRgb;
    } else if (!strcmp(beh, "thinking")) {
      bool first = (int)(t * 3) & 1;
      a = scale(iris, first ? 1.f : 0.15f);
      b = scale(iris, first ? 0.15f : 1.f);
    } else if (ph == live::RESPONDING) {
      a = b = scale(iris, 0.25f + 0.75f * mic::speakLevel());
    } else if (ph != live::IDLE) {
      a = b = scale(iris, 0.3f + 0.5f * pulse);
    } else if (privacy) {
      a = b = 0x300000;
    } else if (mode == 2 && !eyes::asleep()) {
      a = b = scale(iris, 0.12f + 0.04f * sinf(t * 1.3f));
    }
  }
  // WS2812 at full power is glaring; at night dimmer, with the display gone dark off as well
  float k = displayDark ? 0.f : bright / 100.f * (night ? 0.3f : 1.f) * 0.6f;
  // smooth fades (alarm and thinking switch hard on purpose)
  static float cur[6];
  uint32_t tgt[2] = {scale(a, k), scale(b, k)};
  bool hard = ringUntil || !strcmp(eyes::currentName(), "thinking");
  for (int i = 0; i < 6; ++i) {
    float v = (tgt[i / 3] >> (16 - 8 * (i % 3)) & 255);
    cur[i] = hard ? v : cur[i] + (v - cur[i]) * 0.2f;
  }
  auto pack = [&](int o) { return (uint32_t)cur[o] << 16 | (uint32_t)cur[o + 1] << 8 | (uint32_t)cur[o + 2]; };
  leds::set(pack(0), pack(3));
}

// ============================================================== setup screen, house keeping
// WiFi setup: a QR code to join the robot's access point; once connected one with its address
static void setupScreen() {
  static char shown[96] = "";
  char key[96];
  if (menuOpen) {
    if (esp_timer_get_time() - menuAt > 15000000) menuClose();  // nothing chosen: back to the eyes
    shown[0] = 0;
    return;
  }
  if (!net::setupActive()) {
    if (shown[0]) screen::hide();
    shown[0] = 0;
    return;
  }
  bool joined = net::joinStatus() == 2 && net::connected();
  snprintf(key, sizeof(key), "%d%s%s", joined, net::setupSsid(), net::ip());
  if (!strcmp(key, shown)) return;
  snprintf(shown, sizeof(shown), "%s", key);
  char qr[120], l1[40], l2[40];
  if (joined) {
    snprintf(qr, sizeof(qr), "http://%s/", net::ip());
    snprintf(l1, sizeof(l1), "%s", net::ip());
    screen::show("Verbunden!", qr, l1, "pixel.local");
  } else {
    snprintf(qr, sizeof(qr), "WIFI:T:WPA;S:%s;P:%s;;", net::setupSsid(), net::setupPass());
    snprintf(l1, sizeof(l1), "%s", net::setupSsid());
    snprintf(l2, sizeof(l2), "Passwort %s", net::setupPass());
    screen::show("WLAN einrichten", qr, l1, l2, "dann 192.168.4.1");
  }
}

static void houseKeeping(int64_t now) {
  ringTick(now);
  ledsTick(now);
  // lamps changed in Home Assistant: the mood light follows one of them, others show on the eyes
  static ha::LightEvent le;
  EXT_RAM_BSS_ATTR static settings::Conv c;
  while (ha::popLight(le)) {
    settings::conv(c);
    if (!strcmp(le.entity, c.moodLight)) eyes::moodLight(le.on, le.rgb);
    if (le.change && !live::active() && !eyes::asleep() && !eyes::reacting()) {
      eyes::home(le.change == 1 ? "light_on" : le.change == 2 ? "light_off" : "light_color", le.rgb);
      lampRgb = le.change == 2 ? 0 : le.rgb;
      lampUntil = now + 3000000;
    }
  }
  static int64_t slow = 0;
  if (now < slow) return;
  slow = now + 10000000;
  // night mode starts / ends with the clock
  settings::conv(c);
  time_t t = time(nullptr);
  struct tm tmv;
  localtime_r(&t, &tmv);
  int h = tmv.tm_hour;
  bool n = c.nightOn && t > 1700000000 &&
           (c.nightFrom > c.nightTo ? (h >= c.nightFrom || h < c.nightTo) : (h >= c.nightFrom && h < c.nightTo));
  if (n != night) appliedVersion = 0;  // re-apply the settings (brightness, idle pauses)
  static int lastSched = -2;  // the schedule reached its next step
  int sb = -1, sv = -1;
  int sched = t > 1700000000 && settings::scheduled(h * 60 + tmv.tm_min, sb, sv) ? sb * 1000 + sv : -1;
  if (sched != lastSched) {
    if (lastSched != -2) appliedVersion = 0;
    lastSched = sched;
  }
  if (!c.moodLight[0]) eyes::moodLight(false, 0);
  // battery: plugged in -> the charge on the eyes; getting low -> once at 20 % and at 10 %
  battery::tick();
  int bl = battery::level();
  bool bc = battery::charging();
  static bool wasCharging = false;
  static int warned = 101;
  bool calm = !live::active() && !eyes::asleep() && !eyes::reacting();
  int step = bl <= 10 ? 10 : bl <= 20 ? 20 : 101;  // the warning threshold reached
  bool low = bl >= 0 && !bc && step < warned, plugged = bl >= 0 && bc && !wasCharging;
  bool show = calm && (low || plugged);
  if (low && calm) warned = step;
  if (bc || bl > 30) warned = 101;
  eyes::battery(bl, bc, show);
  if (bl >= 0) wasCharging = bc;
  // good morning: the first wake-up between 5 and 11 o'clock shows the weather
  static bool wasAsleep = false;
  static int greetedDay = -1;
  bool asleep = eyes::asleep();
  if (wasAsleep && !asleep && h >= 5 && h < 11 && greetedDay != tmv.tm_yday && ha::configured(c)) {
    greetedDay = tmv.tm_yday;
    live::requestWeather();
  }
  wasAsleep = asleep;
}

// ============================================================== main loop
static void frameTask(void*) {
  // black screen first, then fade the backlight in
  clearMode = true;
  drawRect({0, 0, 240, 240});
  clearMode = false;

  int64_t last = esp_timer_get_time(), statT = last, bootT = last, statusT = last;
  float lastFps = 0;
  int frames = 0;
  int64_t workUs = 0, maxWork = 0, updUs = 0;
  long area = 0;
  for (;;) {
    int64_t now = esp_timer_get_time();
    float dt = (now - last) * 1e-6f;
    last = now;

    pollInputs();
    int64_t p0 = esp_timer_get_time();
    setupScreen();
    houseKeeping(now);
    int64_t u0 = esp_timer_get_time();
    eyes::update(dt);
    updUs += esp_timer_get_time() - u0;
    eyes::Rect r = eyes::dirtyRect();
    if (screen::active()) r = {0, 0, 0, 0};  // the setup page stays until it changes
    static uint32_t screenVer = 0;
    if (screen::version() != screenVer) {  // the setup page appeared / changed / went away: whole screen
      screenVer = screen::version();
      r = {0, 0, 240, 240};
    }
    // display off timeout: nobody there for a while -> backlight fades out and drawing pauses
    if (live::active() || ringUntil || menuOpen || screen::active()) activity();
    if (!lastActive) lastActive = now;
    int offMin = settings::displayOffMin();
    bool dark = offMin > 0 && now - lastActive > (int64_t)offMin * 60000000;
    displayDark = dark;
    static bool paused = false;
    if (dark && backlight <= 0.f) {
      paused = true;
    } else {
      if (paused) r = {0, 0, 240, 240};  // back on: the whole screen, it was not drawn meanwhile
      paused = false;
      if (!r.empty()) drawRect(r);
    }

    float since = (now - bootT) * 1e-6f;
    static bool valid = false;  // 10 s of eyes: this firmware works, no rollback to the previous one
    if (!valid && since > 10.f) {
      valid = true;
      esp_ota_mark_app_valid_cancel_rollback();
    }
    static uint32_t seenVer = 0;
    if (settings::version() != seenVer) {  // changed in the web interface: show it
      seenVer = settings::version();
      activity();
    }
    if (settings::version() != appliedVersion) applySettings();
    // backlight: fades in after boot, then follows brightness / schedule / timeout smoothly
    float tgt = dark ? 0.f : brightness;
    if (since < 1.2f)
      backlight = brightness * (since < 0.15f ? 0.f : (since - 0.15f) / 1.05f);
    else
      backlight = fabsf(tgt - backlight) < 0.003f ? tgt : backlight + (tgt - backlight) * fminf(1.f, dt * (dark ? 1.5f : 4.f));
    backlightSet(backlight);
    char cmd[40];
    while (web::popCommand(cmd, sizeof(cmd))) {
      activity();
      if (!strcmp(cmd, "wake")) {
        startLive(false);
      } else if (!strcmp(cmd, "chat")) {
        startLive(false);
      } else if (!strcmp(cmd, "play:ringstop")) {
        ringStop(false);
      } else if (!strncmp(cmd, "play:", 5)) {
        int seq[6], sn = settings::custom(cmd + 5, seq);
        if (sn)
          eyes::sequence(seq, sn);
        else
          eyes::trigger(eyes::behaviorIndex(cmd + 5));
      }
    }
    if (now - statusT > 1000000) {
      statusT = now;
      publishStatus(lastFps);
    }

    int64_t spent = esp_timer_get_time() - now;
    if (spent > 35000)  // where a slow frame lost its time (inputs + conversation events, house keeping, rest)
      printf("slow frame %.1f ms: inputs %.1f, house %.1f, update+draw+rest %.1f\n", spent / 1000.f, (p0 - now) / 1000.f,
             (u0 - p0) / 1000.f, (esp_timer_get_time() - u0) / 1000.f);
    workUs += spent;
    if (spent > maxWork) maxWork = spent;
    area += (long)(r.x1 - r.x0) * (r.y1 - r.y0);
    frames++;
    if (now - statT > 3000000) {
      float s = (now - statT) * 1e-6f;
      float mConf, mBpm, mPos, mLoud, mDrive;
      mic::music(mConf, mBpm, mPos, mLoud, mDrive);
      lastFps = frames / s;
      printf("fps %.1f | work avg %.1f ms max %.1f ms | update %.1f | render %.1f wait %.1f ms (both cores) | "
             "core 0 %d bands, %d taken back | area %ld px | music %.2f %.0f bpm drive %.2f loud %.2f | %s | %s\n",
             frames / s, workUs / 1000.f / frames, maxWork / 1000.f, updUs / 1000.f / frames, statRender.load() / 1000.f / frames,
             statWait.load() / 1000.f / frames, statHelped.exchange(0), statRescued.exchange(0), area / frames, mConf, mBpm, mDrive, mLoud, eyes::currentName(), eyes::colorwayName());
      statRender = 0;
      statWait = 0;
      statT = now;
      frames = 0;
      workUs = maxWork = updUs = 0;
      area = 0;
    }
    // even pacing: frames start on a fixed grid (1 ms ticks + a short spin), no catching up after a slow one
    static int64_t due = 0;
    int64_t end = esp_timer_get_time();
    due = end - due > frameUs ? end : due + frameUs;
    if (due - end >= 1000) vTaskDelay(pdMS_TO_TICKS((due - end) / 1000));
    else vTaskDelay(1);  // lets the low priority tasks on this core run
    while (esp_timer_get_time() < due) {
    }
  }
}

extern "C" void app_main(void) {
  // keep the board powered on battery, keep the speaker amp off
  gpio_config_t g = {};
  g.pin_bit_mask = (1ULL << PIN_POWER_HOLD) | (1ULL << PIN_AUDIO_PA);
  g.mode = GPIO_MODE_OUTPUT;
  gpio_config(&g);
  gpio_set_level(PIN_POWER_HOLD, 1);
  gpio_set_level(PIN_AUDIO_PA, 0);
  gpio_config_t bt = {};
  bt.pin_bit_mask = 1ULL << PIN_BOOT;
  bt.mode = GPIO_MODE_INPUT;
  bt.pull_up_en = GPIO_PULLUP_ENABLE;
  gpio_config(&bt);

  usb_serial_jtag_driver_config_t uc = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
  usb_serial_jtag_driver_install(&uc);

  backlightInit();
  lcdInit();

  freeQ = xQueueCreate(NBUF, sizeof(uint16_t*));
  flightQ = xQueueCreate(NBUF, sizeof(uint16_t*));
  sendQ = xQueueCreate(NBUF, sizeof(Job));
  for (int i = 0; i < NBUF; ++i) {
    uint16_t* b = (uint16_t*)heap_caps_malloc(BAND_PIX * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    assert(b);
    xQueueSend(freeQ, &b, 0);
  }

  // cJSON trees (settings, session messages, Home Assistant answers) live in PSRAM
  cJSON_Hooks hooks = {[](size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }, free};
  cJSON_InitHooks(&hooks);
  sd::mount();
  settings::init();
  assist::init();
  touchInit();
  leds::init();
  battery::init();
  if (CFG_MIC_ENABLE && !mic::start()) ESP_LOGW(TAG, "microphone unavailable, listening animation disabled");
  net::start();
  live::start();
  eyes::init(esp_random());
  applySettings();
  web::start();
  ha::startEvents();
  ESP_LOGI(TAG, "starting, send 'h' over serial for commands");

  // stacks sized from measurements (free at the high-water mark: lcd 2.9 / render0 5.0 / frame 5.7 of
  // 4 / 6 / 8 KB) with margin: internal RAM is needed by WiFi, TLS and the LED driver
  xTaskCreatePinnedToCore(senderTask, "lcd_send", 3072, nullptr, 7, nullptr, 0);
  helperQ = xQueueCreate(1, sizeof(FrameJob));
  // below the WebSocket / web server (5): core 0 gives the eyes only the time it has to spare
  xTaskCreatePinnedToCore(workerTask, "render0", 3584, nullptr, 4, &workerTaskH, 0);
  xTaskCreatePinnedToCore(frameTask, "frame", 6144, nullptr, 5, &frameTaskH, 1);
}
