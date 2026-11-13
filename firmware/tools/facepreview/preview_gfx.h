// Stand-in for the few LovyanGFX calls face.cpp uses, drawing into a framebuffer, so faces
// can be previewed on a computer. Pixel rules are close to LovyanGFX, not identical.
#pragma once
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <algorithm>
using std::max;
using std::min;

struct PreviewGfx {
  static const int W = 240, H = 320;
  uint16_t fb[W * H];
  void px(int x, int y, uint16_t c) { if (x >= 0 && y >= 0 && x < W && y < H) fb[y * W + x] = c; }
  void startWrite() {}
  void endWrite() {}
  void fillRect(int x, int y, int w, int h, uint16_t c) {
    for (int j = y; j < y + h; j++) for (int i = x; i < x + w; i++) px(i, j, c);
  }
  void fillCircle(int cx, int cy, int r, uint16_t c) { fillEllipse(cx, cy, r, r, c); }
  void fillEllipse(int cx, int cy, int rx, int ry, uint16_t c) {
    for (int j = -ry; j <= ry; j++)
      for (int i = -rx; i <= rx; i++)
        if ((float)i * i / ((float)rx * rx + 0.5f) + (float)j * j / ((float)ry * ry + 0.5f) <= 1.0f) px(cx + i, cy + j, c);
  }
  void fillTriangle(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t c) {
    int minx = min(x0, min(x1, x2)), maxx = max(x0, max(x1, x2));
    int miny = min(y0, min(y1, y2)), maxy = max(y0, max(y1, y2));
    auto e = [](int ax, int ay, int bx, int by, int x, int y) { return (bx - ax) * (y - ay) - (by - ay) * (x - ax); };
    for (int y = miny; y <= maxy; y++)
      for (int x = minx; x <= maxx; x++) {
        int a = e(x0, y0, x1, y1, x, y), b = e(x1, y1, x2, y2, x, y), d = e(x2, y2, x0, y0, x, y);
        if ((a >= 0 && b >= 0 && d >= 0) || (a <= 0 && b <= 0 && d <= 0)) px(x, y, c);
      }
  }
  // Angles in degrees, 0 = right, clockwise (y down), like LovyanGFX.
  void fillEllipseArc(int cx, int cy, int r0x, int r1x, int r0y, int r1y, float a0, float a1, uint16_t c) {
    for (int j = -r1y; j <= r1y; j++)
      for (int i = -r1x; i <= r1x; i++) {
        float o = (float)i * i / ((float)r1x * r1x + 0.5f) + (float)j * j / ((float)r1y * r1y + 0.5f);
        float in = (r0x && r0y) ? (float)i * i / ((float)r0x * r0x) + (float)j * j / ((float)r0y * r0y) : 2;
        if (o > 1.0f || in < 1.0f) continue;
        float a = atan2f((float)j, (float)i) * 180 / (float)M_PI;
        if (a < 0) a += 360;
        if (a0 <= a1 ? (a >= a0 && a <= a1) : (a >= a0 || a <= a1)) px(cx + i, cy + j, c);
      }
  }
  void fillArc(int cx, int cy, int r0, int r1, float a0, float a1, uint16_t c) { fillEllipseArc(cx, cy, r0, r1, r0, r1, a0, a1, c); }
  void fillRoundRect(int x, int y, int w, int h, int r, uint16_t c) {
    fillRect(x + r, y, w - 2 * r, h, c);
    fillRect(x, y + r, w, h - 2 * r, c);
    fillCircle(x + r, y + r, r, c);
    fillCircle(x + w - r - 1, y + r, r, c);
    fillCircle(x + r, y + h - r - 1, r, c);
    fillCircle(x + w - r - 1, y + h - r - 1, r, c);
  }
};
extern PreviewGfx tft;
