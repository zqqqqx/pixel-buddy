// Credentials built into the firmware. All optional: the setup (WiFi access point + QR code on the
// display) asks for WiFi and the OpenAI key. Build with -DPIXEL_PUBLIC for a firmware to share.
#pragma once
#if __has_include("secrets.h") && !defined(PIXEL_PUBLIC)
#include "secrets.h"
#endif
#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif
#ifndef WIFI_PASS
#define WIFI_PASS ""
#endif
#ifndef OPENAI_API_KEY
#define OPENAI_API_KEY ""
#endif
