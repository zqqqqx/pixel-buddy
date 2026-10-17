// SD card (SPI mode: SCK 17, MOSI 18, MISO 21, CS 13), mounted at /sdcard.
#pragma once
#include <stdint.h>

namespace sd {

bool mount();
bool ok();
bool append(const char* path, const char* text);   // append text to a file (in the background)
bool writeLater(const char* path, const char* text);  // replace a file's content (in the background)
long readTail(const char* path, char* dst, long cap);  // last cap-1 bytes of a file (0-terminated)
// 16 kHz mono WAV as <dir>/<prefix>_NNN.wav (next free number); the file name goes to name (may be null)
bool saveWav(const char* dir, const char* prefix, const int16_t* pcm, int n, char* name, int cap);
bool saveWavLater(const char* dir, const char* prefix, const int16_t* pcm, int n);  // same, in the background
int countFiles(const char* dir, const char* ext);  // files ending in ext (0 without card / folder)

}  // namespace sd
