// PC check of src/kws.cpp: raw 16 kHz int16 on stdin -> "frame prob hit" per network evaluation
#include <cstdio>
#include <vector>
#include "../src/kws.h"
int main() {
  kws::init();
  std::vector<int16_t> buf(320);
  long frames = 0;
  float last = -1;
  size_t n;
  while ((n = fread(buf.data(), 2, buf.size(), stdin)) > 0) {
    bool hit = kws::feed(buf.data(), (int)n);
    frames += n / 320;
    if (kws::lastProb() != last || hit) {
      last = kws::lastProb();
      printf("%ld %.5f %d\n", frames, last, hit ? 1 : 0);
    }
  }
}
