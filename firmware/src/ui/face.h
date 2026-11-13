// Zuse's faces as vectors: FUD's face art (thoughtfix, GPL-3.0) traced into facevec_data.h,
// plus a few extra faces drawn with shapes on the traced patch. About 7.6 KB of flash, no
// RAM, any size.
#pragma once
#ifdef FACE_PREVIEW
#include "preview_gfx.h"
#else
#include <Arduino.h>
#endif

namespace face {
// Draw `name` filling a size x size square at (x, y), cleared to bg first.
// Unknown names draw the default smile. Returns false for an unknown name.
bool draw(const char* name, int x, int y, int size, uint16_t bg);
// Idle animation frame: features turned by `angle` degrees (the rim stays, like a wheel) and a
// radar beam at `sweep` degrees (< 0 = none). Traced faces only: returns false, drawing
// nothing, for the shape-drawn extras (camera, alarms).
bool drawAnim(const char* name, int x, int y, int size, uint16_t bg, float angle, float sweep);
}
