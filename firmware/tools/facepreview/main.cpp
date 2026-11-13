// Renders Zuse's drawn faces to a PNG contact sheet, no board needed.
//   tools/facepreview/run.sh            all faces > /tmp... path printed
// Builds src/ui/face.cpp with FACE_PREVIEW so the art code is the same as on the device.
#include <stdio.h>
#include <stdlib.h>
#include "preview_gfx.h"
#include "../../src/ui/face.h"

PreviewGfx tft;

static const char* NAMES[] = {"defaultsmile", "glance-left", "glance-right", "winkingface", "yawning-face",
                              "total-blank-face", "sadface", "grinningface", "neutralface", "shockedface",
                              "tongueface", "redface", "heartface", "swirly-face", "abused-face",
                              "camera", "alarm-attack", "alarm-hacker"};

// Idle animation frames, after the faces: wheel turn angles, then radar sweep angles.
struct Frame { const char* name; float angle, sweep; };
static const Frame FRAMES[] = {{"defaultsmile", 30, -1},  {"defaultsmile", 90, -1}, {"defaultsmile", 180, -1},
                               {"grinningface", 240, -1}, {"defaultsmile", 0, 300},  {"defaultsmile", 0, 45},
                               {"sadface", 0, 160}};

int main(int argc, char** argv) {
  const char* out = argc > 1 ? argv[1] : "faces.ppm";
  const int NF = sizeof(NAMES) / sizeof(NAMES[0]), NA = sizeof(FRAMES) / sizeof(FRAMES[0]);
  const int N = NF + NA, COLS = 5, CELL = 170, S = 162;
  int rows = (N + COLS - 1) / COLS, ow = COLS * CELL, oh = rows * CELL;
  unsigned char* img = (unsigned char*)calloc(ow * oh * 3, 1);
  for (int k = 0; k < N; k++) {
    if (k < NF) {
      bool alarm = !strncmp(NAMES[k], "alarm", 5);
      face::draw(NAMES[k], 0, 0, S, alarm ? 0xF800 : 0x0000);
    } else {
      const Frame& fr = FRAMES[k - NF];
      face::drawAnim(fr.name, 0, 0, S, 0x0000, fr.angle, fr.sweep);
    }
    int ox = (k % COLS) * CELL + 4, oy = (k / COLS) * CELL + 4;
    for (int y = 0; y < S; y++)
      for (int x = 0; x < S; x++) {
        uint16_t c = tft.fb[y * PreviewGfx::W + x];
        unsigned char* p = img + ((oy + y) * ow + ox + x) * 3;
        p[0] = (c >> 11) << 3;
        p[1] = ((c >> 5) & 63) << 2;
        p[2] = (c & 31) << 3;
      }
  }
  FILE* f = fopen(out, "wb");
  fprintf(f, "P6 %d %d 255\n", ow, oh);
  fwrite(img, 1, ow * oh * 3, f);
  fclose(f);
  printf("%s\n", out);
  return 0;
}
