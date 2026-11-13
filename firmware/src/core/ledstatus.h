// The LED language (decided): A = BLE, B = Wi-Fi, C = sub-GHz, D = 2.4 GHz.
// A detection flashes its radio's LED in the category color. An active attack pulses all
// four red until acknowledged. Muted: no flashes, but the attack pulse stays (like FUD,
// mute silences, it does not hide a live attack).
#pragma once
#include <Arduino.h>

namespace ledstatus {
void flash(char src, uint8_t r, uint8_t g, uint8_t b, uint32_t ms = 1500);
void flashCat(char src, uint8_t cat, uint32_t ms = 1500);
void tick();
}
