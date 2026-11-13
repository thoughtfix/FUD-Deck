// D-pad (PCF8574 over I2C) and touch, turned into simple events for the UI.
//
// D-pad, standard behavior:
//   UP/DOWN      fire on press, auto-repeat while held
//   RIGHT        fires on press
//   LEFT         short press = Left, hold = Back
//   CENTER       short press = Center, hold = HoldCenter
// Touch: a Tap(x, y) in screen pixels when the finger lifts. dx/dy are how far the finger
// travelled while down: a small move is a tap, a long one is a swipe.
#pragma once
#include <Arduino.h>

enum class Key : uint8_t { None, Up, Down, Left, Right, Center, Back, HoldCenter };

struct Tap {
  int16_t x, y;      // where the finger came down
  int16_t dx, dy;    // travel while down
  bool isSwipe() const { return abs(dx) > 50 || abs(dy) > 40; }
};

// Raw XPT2046 readings at the screen edges (x=0, x=240, y=0, y=320).
struct TouchCal { int16_t xl, xr, yt, yb; };

namespace input {
void begin();                  // Wire on the board I2C pins at 400 kHz, PCF8574 to inputs
void poll();                   // call often from the UI loop
Key nextKey();                 // Key::None when empty
bool nextTap(Tap& t);
void inject(Key k);            // remote control (dev builds)
void injectTap(int16_t x, int16_t y);
void injectSwipe(int16_t x, int16_t y, int16_t dx, int16_t dy);
bool buttonsPresent();         // PCF8574 answered
int rawButtons();              // last PCF8574 byte (bit low = pressed), -1 if none read
const char* keyName(Key k);
TouchCal touchCal();
void setTouchCal(const TouchCal& c, bool save);   // save = write to NVS
void resetTouchCal();                             // back to the measured default, NVS cleared
bool lastRaw(int32_t& x, int32_t& y);             // raw reading behind the last tap
}
