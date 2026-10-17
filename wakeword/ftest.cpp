#include <cstdio>
#include <vector>
#include "../src/kws.h"
int main() {  // print every frame's 40 log-mel values
  kws::init();
  std::vector<int16_t> b(320);
  while (fread(b.data(), 2, 320, stdin) == 320) {
    kws::feed(b.data(), 320);
    const float* f = kws::lastFrame();
    for (int i = 0; i < 40; ++i) printf("%.5f ", f[i]);
    printf("\n");
  }
}
