// WiFi station (auto reconnect) + wall clock, and the setup access point with captive portal.
#pragma once
#include <stdint.h>

namespace net {

void start();         // connect WiFi in the background (opens the setup when no network is known)
bool connected();
const char* ip();
int rssi();
const char* ssid();   // the configured network

// setup: own access point "Pixel-Setup-XXXX" (WPA2, random password) until a network works
bool setupActive();
const char* setupSsid();
const char* setupPass();
void forgetWifi();                                // factory reset: the network from the setup is erased
void stopSetup();                                 // close the access point (only while online)
bool saveWifi(const char* ssid, const char* pass);  // store and connect, false = invalid input
int joinStatus();                                 // 0 idle, 1 connecting, 2 connected, 3 failed
int scan(char* jsonOut, int cap);                 // nearby networks as a JSON array, strongest first

// captive portal DNS: length of a query up to the end of its first question, -1 if malformed
inline int dnsQuestionEnd(const uint8_t* b, int n) {
  int i = 12;
  while (i < n && b[i]) {
    if (b[i] >= 0xC0) return -1;  // no compression in a question
    i += b[i] + 1;
  }
  return i + 5 <= n ? i + 5 : -1;  // the 0 byte, type, class
}

}  // namespace net
