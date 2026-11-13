#include "input.h"
#include <Wire.h>
#include <Preferences.h>
#include "display.h"
#include "pins.h"

namespace input {

static const uint8_t PCF_ADDR = 0x20;     // A0-A2 tied low on V2
static const uint32_t POLL_MS = 10;
static const uint8_t STABLE_POLLS = 3;    // 30 ms debounce (measured ~20 ms bounce)
static const uint32_t HOLD_LEFT_MS = 600;
static const uint32_t HOLD_CENTER_MS = 800;
static const uint32_t REPEAT_DELAY_MS = 400;
static const uint32_t REPEAT_MS = 120;

static bool present = false;

// Small ring buffers. The UI drains them every loop.
static Key keyQ[16];
static uint8_t kHead = 0, kTail = 0;
static Tap tapQ[6];
static uint8_t tHead = 0, tTail = 0;

static void pushKey(Key k) {
  uint8_t n = (kHead + 1) % 16;
  if (n == kTail) return;  // full: drop rather than block
  keyQ[kHead] = k;
  kHead = n;
}

// Touch calibration: raw XPT2046 values at the screen edges. Defaults are our measured unit
// (pins.h). Settings > Touch replaces them and saves to NVS.
static TouchCal cal = {TOUCH_RAW_X_LEFT, TOUCH_RAW_X_RIGHT, TOUCH_RAW_Y_TOP, TOUCH_RAW_Y_BOTTOM};
static int32_t lastRawX = 0, lastRawY = 0;
static bool haveRaw = false;

static void pushTap(int16_t x, int16_t y, int16_t dx = 0, int16_t dy = 0) {
  uint8_t n = (tHead + 1) % 6;
  if (n == tTail) return;
  tapQ[tHead] = {x, y, dx, dy};
  tHead = n;
}

Key nextKey() {
  if (kHead == kTail) return Key::None;
  Key k = keyQ[kTail];
  kTail = (kTail + 1) % 16;
  return k;
}

bool nextTap(Tap& t) {
  if (tHead == tTail) return false;
  t = tapQ[tTail];
  tTail = (tTail + 1) % 6;
  return true;
}

void inject(Key k) { pushKey(k); }
static int32_t unmap(int16_t v, int32_t rawAtZero, int32_t rawAtMax, int16_t size) {
  return rawAtZero + (int32_t)v * (rawAtMax - rawAtZero) / size;
}
void injectTap(int16_t x, int16_t y) {
  // Fake the raw reading too, so the calibration screen can be driven remotely.
  lastRawX = unmap(x, cal.xl, cal.xr, 240);
  lastRawY = unmap(y, cal.yt, cal.yb, 320);
  haveRaw = true;
  pushTap(x, y);
}
void injectSwipe(int16_t x, int16_t y, int16_t dx, int16_t dy) { pushTap(x, y, dx, dy); }
bool buttonsPresent() { return present; }

const char* keyName(Key k) {
  switch (k) {
    case Key::Up: return "up";
    case Key::Down: return "down";
    case Key::Left: return "left";
    case Key::Right: return "right";
    case Key::Center: return "center";
    case Key::Back: return "back";
    case Key::HoldCenter: return "holdcenter";
    default: return "none";
  }
}

void begin() {
  Wire.begin(PIN_SDA, PIN_SCL, 400000);
  Wire.beginTransmission(PCF_ADDR);
  Wire.write(0xFF);  // quasi-bidirectional pins high = inputs
  present = Wire.endTransmission() == 0;
  Preferences p;
  if (p.begin("touch", true)) {
    if (p.isKey("cal") && p.getBytesLength("cal") == sizeof(TouchCal)) p.getBytes("cal", &cal, sizeof(cal));
    p.end();
  }
}

TouchCal touchCal() { return cal; }
void setTouchCal(const TouchCal& c, bool save) {
  cal = c;
  if (!save) return;
  Preferences p;
  if (p.begin("touch", false)) { p.putBytes("cal", &cal, sizeof(cal)); p.end(); }
}
void resetTouchCal() {
  cal = {TOUCH_RAW_X_LEFT, TOUCH_RAW_X_RIGHT, TOUCH_RAW_Y_TOP, TOUCH_RAW_Y_BOTTOM};
  Preferences p;
  if (p.begin("touch", false)) { p.remove("cal"); p.end(); }
}
bool lastRaw(int32_t& x, int32_t& y) {
  x = lastRawX;
  y = lastRawY;
  return haveRaw;
}

static int lastPcf = -1;   // last PCF8574 byte read, for the button test
static int readPcf() {
  if (Wire.requestFrom((int)PCF_ADDR, 1) != 1) return -1;
  return lastPcf = Wire.read();
}
int rawButtons() { return lastPcf; }

// ---- D-pad ----

struct Btn {
  uint8_t bit;
  bool down = false;
  uint8_t stable = 0;
  bool raw = false;
  uint32_t since = 0;
  uint32_t lastRepeat = 0;
  bool holdFired = false;
};
static Btn bUp{PCF_BIT_UP}, bDown{PCF_BIT_DOWN}, bLeft{PCF_BIT_LEFT}, bRight{PCF_BIT_RIGHT}, bCenter{PCF_BIT_SELECT};

// Returns true on a debounced change.
static bool debounce(Btn& b, int pcf, uint32_t now) {
  bool rawDown = !(pcf & (1 << b.bit));  // active low
  if (rawDown != b.raw) { b.raw = rawDown; b.stable = 0; return false; }
  if (b.stable < STABLE_POLLS) b.stable++;
  if (b.stable == STABLE_POLLS && b.down != rawDown) {
    b.down = rawDown;
    b.since = now;
    b.lastRepeat = now;
    b.holdFired = false;
    return true;
  }
  return false;
}

static void pollButtons(uint32_t now) {
  int v = readPcf();
  if (v < 0) return;

  // UP/DOWN: press fires, then auto-repeat.
  for (Btn* b : {&bUp, &bDown}) {
    Key k = (b == &bUp) ? Key::Up : Key::Down;
    if (debounce(*b, v, now) && b->down) pushKey(k);
    if (b->down && now - b->since > REPEAT_DELAY_MS && now - b->lastRepeat > REPEAT_MS) {
      b->lastRepeat = now;
      pushKey(k);
    }
  }
  if (debounce(bRight, v, now) && bRight.down) pushKey(Key::Right);

  // LEFT: hold = Back, short = Left (decided on release).
  if (debounce(bLeft, v, now) && !bLeft.down && !bLeft.holdFired) pushKey(Key::Left);
  if (bLeft.down && !bLeft.holdFired && now - bLeft.since >= HOLD_LEFT_MS) {
    bLeft.holdFired = true;
    pushKey(Key::Back);
  }

  // CENTER: hold = HoldCenter, short = Center.
  if (debounce(bCenter, v, now) && !bCenter.down && !bCenter.holdFired) pushKey(Key::Center);
  if (bCenter.down && !bCenter.holdFired && now - bCenter.since >= HOLD_CENTER_MS) {
    bCenter.holdFired = true;
    pushKey(Key::HoldCenter);
  }
}

// ---- touch ----

static bool touching = false;
static int32_t sumX = 0, sumY = 0;
static int samples = 0;
static int32_t firstX = 0, firstY = 0, lastX = 0, lastY = 0;   // raw, for swipes

static int16_t mapAxis(int32_t raw, int32_t rawAtZero, int32_t rawAtMax, int16_t size) {
  int32_t v = (rawAtZero - raw) * size / (rawAtZero - rawAtMax);
  if (v < 0) v = 0;
  if (v > size - 1) v = size - 1;
  return (int16_t)v;
}

static void pollTouch() {
  lgfx::touch_point_t tp;
  if (tft.getTouchRaw(&tp, 1)) {
    if (!touching) { firstX = tp.x; firstY = tp.y; }
    touching = true;
    lastX = tp.x;
    lastY = tp.y;
    // Only the first few samples set the tap point. A swipe's end must not drag it.
    if (samples < 4) { sumX += tp.x; sumY += tp.y; }
    samples++;
    return;
  }
  if (!touching) return;
  touching = false;
  if (samples >= 2) {  // one-sample blips are noise
    int n = min(samples, 4);
    int32_t rx = sumX / n, ry = sumY / n;
    int16_t x0 = mapAxis(firstX, cal.xl, cal.xr, 240);
    int16_t y0 = mapAxis(firstY, cal.yt, cal.yb, 320);
    int16_t x1 = mapAxis(lastX, cal.xl, cal.xr, 240);
    int16_t y1 = mapAxis(lastY, cal.yt, cal.yb, 320);
    lastRawX = rx;
    lastRawY = ry;
    haveRaw = true;
    pushTap(mapAxis(rx, cal.xl, cal.xr, 240), mapAxis(ry, cal.yt, cal.yb, 320), x1 - x0, y1 - y0);
  }
  sumX = sumY = 0;
  samples = 0;
}

void poll() {
  static uint32_t last = 0;
  uint32_t now = millis();
  if (now - last < POLL_MS) return;
  last = now;
  if (present) pollButtons(now);
  pollTouch();
}

}  // namespace input
