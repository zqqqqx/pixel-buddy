// "Pixel" wake word: a small own neural network (trained with wakeword/train.py, weights in
// kws_model.h). Streaming: log-mel features every 20 ms, the network looks at the last ~1 s
// every 100 ms. Platform independent (also compiled on the PC to check it against PyTorch).
#pragma once
#include <stdint.h>

namespace kws {

void init();
// feed 16 kHz mono samples; true when the wake word was just detected
// detect = false: only the features (for the music detection while the wake word is off)
bool feed(const int16_t* x, int n, bool detect = true);
float lastProb();               // latest network output 0..1
const float* lastFrame();       // latest log-mel frame (for checks, music detection)
const int BANDS = 40;           // values in a frame
void setSensitivity(float s);   // 0..1, 0.5 = trained threshold, higher = triggers more easily

}  // namespace kws
