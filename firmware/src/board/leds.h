// 4x WS2812 on GPIO1, left to right as held (A..D). GRB.
// Driven through the core RMT API. Adafruit_NeoPixel shows nothing on this core.
#pragma once
#include <Arduino.h>

namespace leds {
constexpr int COUNT = 4;
// Status language: A = BLE, B = Wi-Fi, C = sub-GHz, D = 2.4 GHz. All red = active attack.
enum : uint8_t { BLE = 0, WIFI = 1, SUBGHZ = 2, NRF = 3 };
void begin();
void set(int i, uint8_t r, uint8_t g, uint8_t b);
void fill(uint8_t r, uint8_t g, uint8_t b);
void show();
void clear();
void setLevel(uint8_t level);   // 0 off, 1 low, 2 med, 3 high
uint8_t level();
const char* levelName(uint8_t level);
}
