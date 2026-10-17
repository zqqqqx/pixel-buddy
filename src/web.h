// Web interface: http://pixel.local (settings, animations, look, log).
#pragma once

namespace web {

void start();                              // HTTP server + mDNS (call after WiFi is started)
void setStatus(const char* json);          // status JSON served at /api/status
bool popCommand(char* name, int n);        // "wake" or "play:<animation>" for the main loop

}  // namespace web
