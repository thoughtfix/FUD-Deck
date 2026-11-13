// Desktop harness: runs firmware/facevec.h unchanged into a memory buffer.
// Built by validate.py with -I out -I ../../src/ui
// Run:   out/render_host <size> <outdir>   writes <outdir>/<face>.raw (RGB565, little-endian,
//        transparent pixels = 0x0821, a color no face uses) and prints span counts.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "facevec.h"

static const uint16_t CLEAR = 0x0821;

struct Canvas {
  int w, h;
  std::vector<uint16_t> px;
  long spans = 0, oob = 0, written = 0;
  Canvas(int W, int H) : w(W), h(H), px(W * H, CLEAR) {}
  void startWrite() {}
  void endWrite() {}
  void fillRect(int x, int y, int ww, int hh, uint16_t c) {
    spans++;
    written += (long)ww * hh;
    for (int j = y; j < y + hh; j++)
      for (int i = x; i < x + ww; i++) {
        if (i < 0 || j < 0 || i >= w || j >= h) { oob++; continue; }
        px[j * w + i] = c;
      }
  }
};

int main(int argc, char** argv) {
  if (argc < 3) { fprintf(stderr, "usage: %s size outdir\n", argv[0]); return 1; }
  int size = atoi(argv[1]);
  std::string dir = argv[2];
  for (int i = 0; i < facevec::count(); i++) {
    Canvas c(size, size);
    facevec::draw(c, facevec::name(i), 0, 0, size);
    std::string path = dir + "/" + facevec::name(i) + ".raw";
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) { perror(path.c_str()); return 1; }
    fwrite(c.px.data(), 2, c.px.size(), f);
    fclose(f);
    printf("%s spans=%ld oob=%ld px=%ld\n", facevec::name(i), c.spans, c.oob, c.written);
  }
  return 0;
}
