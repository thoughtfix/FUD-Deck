// Power-on self test. Shown for 2 s at boot, kept for Settings > POST report.
// Features check these results instead of assuming the hardware is there.
#pragma once
#include <Arduino.h>

namespace post {
enum Level : uint8_t { OK, INFO, WARN, FAIL };

struct Item {
  const char* name;
  Level level;
  char detail[40];
};

// Runs every check. onItem (optional) is called as each result lands, for live drawing.
void run(void (*onItem)(int index, const Item& item) = nullptr);
int count();
const Item& item(int i);
const char* levelName(Level l);

// What the rest of the firmware gates on.
bool buttons();
bool sd();
bool nrf(int i);        // 0..2
int nrfCount();
bool cc1101();
bool buzzer();          // Modulino Buzzer (I2C 0x1E) on the QWIIC tap
}
