#include "power.h"
#include <Wire.h>
#include "pins.h"
#include "serial_io.h"

namespace power {

static const uint8_t IP5306_ADDR = 0x75;
static Method meth = Method::None;
static bool ipPresent = false;
static int pct = -1;
static float vbat = 0;
static bool vin = false, full = false;

static int ipRead(uint8_t reg) {
  Wire.beginTransmission(IP5306_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return -1;
  if (Wire.requestFrom((int)IP5306_ADDR, 1) != 1) return -1;
  return Wire.read();
}

static float readIo2Volts() {
  pinMode(PIN_VBAT_SENSE, INPUT);
  analogSetPinAttenuation(PIN_VBAT_SENSE, ADC_11db);
  uint32_t sum = 0;
  for (int i = 0; i < 16; i++) sum += analogReadMilliVolts(PIN_VBAT_SENSE);
  return (sum / 16.0f) * VBAT_RATIO / 1000.0f;
}

// Resting LiPo voltage to percent. Rough, but far better than 4 steps.
static int voltsToPercent(float v) {
  static const float V[] = {4.20, 4.15, 4.11, 4.08, 4.02, 3.98, 3.95, 3.91, 3.87, 3.85, 3.84,
                            3.82, 3.80, 3.79, 3.77, 3.75, 3.73, 3.71, 3.69, 3.61, 3.27};
  const int n = sizeof(V) / sizeof(V[0]);
  if (v >= V[0]) return 100;
  for (int i = 1; i < n; i++) {
    if (v >= V[i]) {
      float f = (v - V[i]) / (V[i - 1] - V[i]);
      return (int)(100 - i * 5 + f * 5 + 0.5f);
    }
  }
  return 0;
}

static bool ipWrite(uint8_t reg, uint8_t v) {
  Wire.beginTransmission(IP5306_ADDR);
  Wire.write(reg);
  Wire.write(v);
  return Wire.endTransmission() == 0;
}

// SYS_CTL0 (0x00) bit 1 = boost keep-on. Stock it is 0: the IP5306 cuts the boost output
// when the load is light, so an idle board can switch itself off. The setting is lost when
// the chip loses power, so it is re-checked now and then.
static int keepOn = -1;   // -1 unknown, 0 failed, 1 set
static void ensureKeepOn() {
  int r = ipRead(0x00);
  if (r < 0) { keepOn = 0; return; }
  if (!(r & 0x02)) ipWrite(0x00, r | 0x02);
  int v = ipRead(0x00);
  keepOn = (v >= 0 && (v & 0x02)) ? 1 : 0;
}

// dev: print the IP5306 control registers.
void dumpRegs() {
  for (uint8_t r : {0x00, 0x01, 0x02, 0x20, 0x21, 0x22, 0x70, 0x71, 0x72, 0x77, 0x78})
    sio::out("[ip5306] 0x%02X = 0x%02X", r, ipRead(r) & 0xFF);
}

static int ipLevel() {
  int r = ipRead(0x78);
  if (r < 0) return -1;
  switch (r & 0xF0) {
    case 0x00: return 100;
    case 0x80: return 75;
    case 0xC0: return 50;
    case 0xE0: return 25;
    default: return 0;
  }
}

static void sample() {
  if (ipPresent) {
    int r70 = ipRead(0x70), r71 = ipRead(0x71);
    vin = r70 >= 0 && (r70 & 0x08);
    full = r71 >= 0 && (r71 & 0x08);
  }
  if (meth == Method::Io2) {
    float v = readIo2Volts();
    vbat = vbat == 0 ? v : vbat * 0.8f + v * 0.2f;  // smooth
    // While charging the cell reads high. Prefer the charger's own idea then.
    pct = (vin && ipPresent) ? max(ipLevel(), voltsToPercent(vbat) - 15) : voltsToPercent(vbat);
    if (pct > 100) pct = 100;
    if (pct < 0) pct = 0;
  } else if (meth == Method::Ip5306) {
    pct = ipLevel();
  }
}

void begin() {
  ipPresent = ipRead(0x70) >= 0;
  if (ipPresent) ensureKeepOn();
  float v = readIo2Volts();
  // A real single cell sits between ~3.0 and ~4.45 V. A clamped IO2 reads ~1 V.
  if (v > 3.0f && v < 4.45f) meth = Method::Io2;
  else if (ipPresent) meth = Method::Ip5306;
  else meth = Method::None;
  sample();
}

void poll() {
  static uint32_t last = 0;
  if (millis() - last < 2000) return;
  last = millis();
  sample();
  static uint32_t lastKeep = 0;
  if (ipPresent && millis() - lastKeep > 60000) { lastKeep = millis(); ensureKeepOn(); }
}

Method method() { return meth; }
const char* methodName() {
  switch (meth) {
    case Method::Io2: return "IO2 divider";
    case Method::Ip5306: return "IP5306 (4 steps)";
    default: return "none";
  }
}
int percent() { return pct; }
float volts() { return meth == Method::Io2 ? vbat : 0; }
bool usb() { return vin; }
bool charging() { return vin && !full; }
bool ip5306Present() { return ipPresent; }
int boostKeepOn() { return keepOn; }

}  // namespace power
