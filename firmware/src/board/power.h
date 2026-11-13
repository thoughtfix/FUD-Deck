// Battery and charge state.
//
// Two methods, chosen at POST because units differ:
//   IO2     battery divider on GPIO2. Our unit: VBAT = IO2 x 1.47. Units with the speaker
//           transistor fitted clamp IO2 near 0.66 V, which fails the plausibility check.
//   IP5306  I2C register 0x78. Works on every unit, but only 4 steps.
#pragma once
#include <Arduino.h>

namespace power {
enum class Method : uint8_t { None, Io2, Ip5306 };
void begin();                 // pick the method (call after input::begin, which starts Wire)
void poll();                  // cheap, rate limited
Method method();
const char* methodName();
int percent();                // -1 unknown
float volts();                // 0 when the method cannot measure voltage
bool usb();                   // external power present
void dumpRegs();              // dev: IP5306 registers to serial
bool charging();              // usb and not full
bool ip5306Present();
int boostKeepOn();          // 1 = IP5306 light-load shutdown disabled, 0 = failed, -1 n/a
}
