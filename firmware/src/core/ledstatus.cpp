#include "ledstatus.h"
#include "../board/leds.h"
#include "detections.h"

namespace ledstatus {

static uint8_t col[leds::COUNT][3];
static uint32_t until[leds::COUNT] = {0};

static int ledFor(char src) {
  switch (src) {
    case 'B': return leds::BLE;
    case 'W': return leds::WIFI;
    case 'S': return leds::SUBGHZ;
    case 'N': return leds::NRF;
    default: return -1;
  }
}

void flash(char src, uint8_t r, uint8_t g, uint8_t b, uint32_t ms) {
  int i = ledFor(src);
  if (i < 0) return;
  col[i][0] = r;
  col[i][1] = g;
  col[i][2] = b;
  until[i] = millis() + ms;
}

// Category colors: tracker purple, camera amber, drone cyan, sub-GHz green, hacker red.
void flashCat(char src, uint8_t cat, uint32_t ms) {
  switch (cat) {
    case det::TRACKER: flash(src, 160, 0, 255, ms); break;
    case det::CAMERA: flash(src, 255, 160, 0, ms); break;
    case det::DRONE: flash(src, 0, 200, 255, ms); break;
    case det::SUBGHZ: flash(src, 0, 255, 0, ms); break;
    default: flash(src, 255, 0, 0, ms); break;
  }
}

void tick() {
  static uint32_t last = 0;
  static uint8_t prev[leds::COUNT][3];
  uint32_t now = millis();
  if (now - last < 40) return;
  last = now;
  uint8_t out[leds::COUNT][3] = {{0}};
  if (det::alarmActive()) {
    bool on = (now / 300) % 2;  // ~1.7 Hz red pulse on all four
    for (int i = 0; i < leds::COUNT; i++) out[i][0] = on ? 255 : 0;
  } else {
    for (int i = 0; i < leds::COUNT; i++)
      if (now < until[i]) memcpy(out[i], col[i], 3);
  }
  if (memcmp(out, prev, sizeof(out)) == 0) return;  // only push changes
  memcpy(prev, out, sizeof(out));
  for (int i = 0; i < leds::COUNT; i++) leds::set(i, out[i][0], out[i][1], out[i][2]);
  leds::show();
}

}  // namespace ledstatus
