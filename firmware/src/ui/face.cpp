#include "face.h"
#include <math.h>
#ifdef FACE_PREVIEW
#include "preview_gfx.h"   // tools/facepreview: same code, drawn into a PNG on a computer
#else
#include "../board/display.h"
#endif
#include "facevec.h"

namespace face {

// FUD's 15 smileys are traced vectors (facevec_data.h). Faces with no source art draw the
// traced disk and patch ("patch-only"), then add an eye and a mouth with shapes, on the
// same 135 x 135 grid.
enum Eye : uint8_t { E_LENS, E_ANGRY, E_SHADES };
enum Mouth : uint8_t { M_SMILE, M_SMIRK, M_GRIT };

struct Extra {
  const char* name;
  Eye eye;
  Mouth mouth;
};

static const Extra EXTRAS[] = {
    {"camera", E_LENS, M_SMILE},           // passive camera hit
    {"alarm-attack", E_ANGRY, M_GRIT},     // live attack: angry, teeth clenched
    {"alarm-hacker", E_SHADES, M_SMIRK},   // hacker device: shades and a smirk
};

static const uint16_t BLACK = 0x0000;
static const uint16_t WHITE = 0xFFFF;

static int ox, oy;
static float sc;
static int X(float v) { return ox + (int)lroundf(v * sc); }
static int Y(float v) { return oy + (int)lroundf(v * sc); }
static int R(float v) { return max(1, (int)lroundf(v * sc)); }

static const float CX = 67.5f;
static const float EYE_X = 43, EYE_Y = 50;   // the visible eye; the patch covers its mirror

// Thick straight stroke with round ends.
static void stroke(float x0, float y0, float x1, float y1, float w, uint16_t c) {
  float dx = x1 - x0, dy = y1 - y0, len = sqrtf(dx * dx + dy * dy);
  if (len < 0.01f) return;
  float nx = -dy / len * w / 2, ny = dx / len * w / 2;
  tft.fillTriangle(X(x0 + nx), Y(y0 + ny), X(x0 - nx), Y(y0 - ny), X(x1 + nx), Y(y1 + ny), c);
  tft.fillTriangle(X(x0 - nx), Y(y0 - ny), X(x1 - nx), Y(y1 - ny), X(x1 + nx), Y(y1 + ny), c);
  tft.fillCircle(X(x0), Y(y0), R(w / 2), c);
  tft.fillCircle(X(x1), Y(y1), R(w / 2), c);
}

// Thick arc with round ends. Angles in degrees, 0 = right, clockwise (screen y is down).
static void arc(float cx, float cy, float r, float w, float a0, float a1, uint16_t c) {
  tft.fillArc(X(cx), Y(cy), R(r - w / 2), R(r + w / 2), a0, a1, c);
  for (float a : {a0, a1}) {
    float t = a * (float)M_PI / 180;
    tft.fillCircle(X(cx + r * cosf(t)), Y(cy + r * sinf(t)), R(w / 2), c);
  }
}

static void eye(Eye e, float x, float y) {
  switch (e) {
    case E_LENS:   // a camera lens for an eye
      tft.fillCircle(X(x), Y(y), R(14), BLACK);
      tft.fillCircle(X(x), Y(y), R(10), 0x4208);
      tft.fillCircle(X(x), Y(y), R(6), BLACK);
      tft.fillCircle(X(x - 3), Y(y - 3), R(2), WHITE);
      break;
    case E_ANGRY:
      stroke(x - 13, y - 2, x + 13, y + 8, 6, BLACK);   // brow, slanting down toward the nose
      tft.fillEllipse(X(x + 1), Y(y + 18), R(6), R(8), BLACK);
      break;
    case E_SHADES:   // one dark lens on a bar, with a glint
      tft.fillRoundRect(X(x - 17), Y(y - 6), R(34), R(22), R(8), BLACK);
      stroke(x + 10, y - 2, x + 30, y - 2, 4, BLACK);
      stroke(x - 10, y + 1, x - 4, y - 3, 2.5f, 0x8410);
      break;
  }
}

static void mouth(Mouth m) {
  switch (m) {
    case M_SMILE: arc(CX, 70, 46, 4.5f, 8, 172, BLACK); break;
    case M_SMIRK: arc(80, 72, 30, 4.5f, 35, 110, BLACK); break;   // one-sided, toward the patch
    case M_GRIT: {   // clenched teeth: a white bar, black outline, tooth gaps
      float cx = 67, cy = 102;
      tft.fillRoundRect(X(cx - 32), Y(cy - 11), R(64), R(22), R(6), BLACK);
      tft.fillRoundRect(X(cx - 28), Y(cy - 7), R(56), R(14), R(4), WHITE);
      tft.fillRect(X(cx - 28), Y(cy - 1), R(56), R(2.5f), BLACK);
      for (int i = -2; i <= 2; i++) tft.fillRect(X(cx + i * 11 - 1), Y(cy - 7), R(2.5f), R(14), BLACK);
      break;
    }
  }
}

bool drawAnim(const char* name, int x, int y, int size, uint16_t bg, float angle, float sweep) {
  if (!facevec::has(name)) return false;
  facevec::draw(tft, name, x, y, size, bg, angle, sweep);
  return true;
}

bool draw(const char* name, int x, int y, int size, uint16_t bg) {
  if (facevec::has(name)) return facevec::draw(tft, name, x, y, size, bg);
  for (auto& e : EXTRAS) {
    if (strcmp(e.name, name)) continue;
    facevec::draw(tft, "patch-only", x, y, size, bg);
    ox = x;
    oy = y;
    sc = size / 135.0f;
    tft.startWrite();
    eye(e.eye, EYE_X, EYE_Y);
    mouth(e.mouth);
    tft.endWrite();
    return true;
  }
  facevec::draw(tft, "defaultsmile", x, y, size, bg);   // unknown name
  return false;
}

}  // namespace face
