#include "leds.h"
#include "pins.h"

namespace leds {

static uint8_t buf[COUNT][3];  // r, g, b
static uint8_t lvl = 1;
static const uint8_t LEVEL_SCALE[4] = {0, 16, 48, 96};  // capped well below full
static bool ready = false;

void begin() {
  ready = rmtInit(PIN_NEOPIXEL, RMT_TX_MODE, RMT_MEM_NUM_BLOCKS_1, 10000000);
  clear();
}

void set(int i, uint8_t r, uint8_t g, uint8_t b) {
  if (i < 0 || i >= COUNT) return;
  buf[i][0] = r;
  buf[i][1] = g;
  buf[i][2] = b;
}

void fill(uint8_t r, uint8_t g, uint8_t b) {
  for (int i = 0; i < COUNT; i++) set(i, r, g, b);
}

void show() {
  if (!ready) return;
  rmt_data_t d[COUNT * 24];
  int k = 0;
  uint8_t scale = LEVEL_SCALE[lvl & 3];
  for (int i = 0; i < COUNT; i++) {
    uint8_t grb[3] = {buf[i][1], buf[i][0], buf[i][2]};
    for (int c = 0; c < 3; c++) {
      uint8_t v = (uint16_t)grb[c] * scale / 255;
      for (int b = 7; b >= 0; b--) {
        bool one = v & (1 << b);
        d[k].level0 = 1;
        d[k].duration0 = one ? 8 : 4;  // 10 MHz ticks: 0.8/0.4 us
        d[k].level1 = 0;
        d[k].duration1 = one ? 4 : 8;
        k++;
      }
    }
  }
  rmtWrite(PIN_NEOPIXEL, d, k, RMT_WAIT_FOR_EVER);
}

void clear() {
  fill(0, 0, 0);
  show();
}

void setLevel(uint8_t level) {
  lvl = level & 3;
  show();
}

uint8_t level() { return lvl; }

const char* levelName(uint8_t level) {
  static const char* n[4] = {"OFF", "LOW", "MED", "HIGH"};
  return n[level & 3];
}

}  // namespace leds
