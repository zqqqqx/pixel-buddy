// ============================================================================
//  Eyes configuration – change values here, rebuild, flash.
// ============================================================================
#pragma once

// ---- look -------------------------------------------------------------------
// Colour scheme at power-on:
//   0 porcelain   (white + blue iris)     5 bubblegum  (pink + white)
//   1 gumball     (red + white)           6 lemon      (yellow + black)
//   2 grape soda  (purple + yellow)       7 ocean      (blue + white)
//   3 marmalade   (orange + yellow)       8 classic    (white + black)
//   4 mint        (mint + navy)           9 lime       (green + black)
#define CFG_COLORWAY 0

// Change the colour scheme by itself every now and then (1 = yes, 0 = never).
// A long press on the BOOT button always switches.
#define CFG_AUTO_COLOR_CHANGE 0

// The two shine dots in the iris when the eyes are idle (0.0 = off .. 1.0 = full).
#define CFG_HIGHLIGHTS 1.0f

// Display brightness 0.0 .. 1.0
#define CFG_BRIGHTNESS 0.85f

// Frame rate cap
#define CFG_MAX_FPS 60

// ---- behaviour ----------------------------------------------------------------
// Pause between the little random animations, in seconds (random in between).
#define CFG_IDLE_MIN_S 3.5f
#define CFG_IDLE_MAX_S 8.5f

// Fall asleep after this many seconds without interaction (0 = never).
// Touch, the BOOT button or the wake word wakes it up again.
#define CFG_SLEEP_AFTER_S 240

// ---- microphone ---------------------------------------------------------------
// 1 = react to voices (listening animation), 0 = microphone off
#define CFG_MIC_ENABLE 1

// How much louder than the room noise speech must be, in dB (higher = less sensitive)
#define CFG_VOICE_SNR_DB 10.0f

// Wake word: 1 = the full listening animation only starts after the wake word,
// 0 = it starts for any detected voice.
#define CFG_WAKEWORD_ENABLE 1

// Wake words besides the own "Pixel" (offline, Espressif WakeNet): packed into the "model" partition (2 MB,
// these take 1.9 MB) and flashed by `pio run -t upload` (an update over WiFi does not change them). Which one
// listens - and so Pixel's name - is chosen in the setup / web interface (settings::WAKES).
// (full list: managed_components/espressif__esp-sr/model/wakenet_model)
#define CFG_WAKEWORD_MODELS "wn10_en_heybuddy wn9_jarvis_tts wn9_computer_tts wn9_sophia_tts wn9_heywillow_tts wn10_de_halloesp"

// Wake word sensitivity: 0 = normal, 1 = more sensitive (more false triggers)
#define CFG_WAKEWORD_AGGRESSIVE 0

// ---- conversation: OpenAI GPT-Live or Realtime (WiFi and API key: setup page or src/secrets.h) ----
// Defaults; everything here can also be changed in the web interface.
#define CFG_ROBOT_NAME "Pixel"
#define CFG_LIVE_MODEL "gpt-live-1"
#define CFG_LIVE_VOICE "marin"
#define CFG_BACKEND_MODEL "gpt-5.4-nano"
// reasoning effort of the backend: none minimal low medium high ("none" makes nano sloppy)
#define CFG_BACKEND_EFFORT "low"
// Speaker volume 0..100
#define CFG_SPEAKER_VOLUME 80

// ---- costs: estimates in USD for the usage display / daily budget (https://openai.com/api/pricing) ---
// Realtime API per 1 million tokens (the "mini" models cost about 0.3 x)
#define CFG_PRICE_RT_AUDIO_IN 32.0
#define CFG_PRICE_RT_AUDIO_OUT 64.0
#define CFG_PRICE_RT_TEXT_IN 4.0
#define CFG_PRICE_RT_TEXT_OUT 16.0
#define CFG_PRICE_RT_CACHED 0.4
// GPT-Live per minute (billed per second), one web search through the backend model
#define CFG_PRICE_LIVE_MIN 0.05
#define CFG_PRICE_SEARCH 0.01
