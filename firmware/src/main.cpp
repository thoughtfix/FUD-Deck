// FUD-Deck: FUD (Find Unwelcome Devices) for small touchscreen ESP32 boards, starting with
// the ESP32-DIV V2 hardware.
// Education and monitoring. Not related to the ESP32-DIV firmware.
// No attack modes or broadcasts enabled by default.
//
// Milestone 4: every radio listening at once. BLE + Wi-Fi (coexistence) and the SPI radios
// (nRF24 pair for jammer correlation, CC1101 on 433 MHz) feed the FUD detection core.
// Core plan: the UI runs here (Arduino loop, core 1) and never waits on a radio.
// Radio work will live in a background task on core 0.

#include <Arduino.h>
#include <Preferences.h>
#include "board/bus.h"
#include "board/display.h"
#include "board/input.h"
#include "board/leds.h"
#include "board/pins.h"
#include "board/power.h"
#include "board/serial_io.h"
#include "core/detections.h"
#include "core/ledstatus.h"
#include "core/scanner.h"
#include "core/rfwatch.h"
#include "core/sdlog.h"
#include "core/clock.h"
#include "board/post.h"
#include "core/zuse.h"
#include "dev/devtools.h"
#include "ui/ui.h"

// Why the chip last started, kept in NVS (last 8, newest last) so a reset on battery can be
// read later over USB. P power on, B brownout, S software, W watchdog, X external,
// U USB (flashing), ! crash, ? other.
static void logReset() {
  esp_reset_reason_t r = esp_reset_reason();
  char c = r == ESP_RST_POWERON ? 'P' : r == ESP_RST_BROWNOUT ? 'B' : r == ESP_RST_SW ? 'S'
         : r == ESP_RST_PANIC ? '!' : r == ESP_RST_EXT ? 'X' : r == ESP_RST_USB ? 'U'
         : (r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_WDT) ? 'W' : '?';
  Preferences p;
  p.begin("fudrst", false);
  String h = p.getString("h", "") + c;
  if (h.length() > 8) h = h.substring(h.length() - 8);
  p.putString("h", h);
  p.end();
  sio::out("[fud] reset %c (%d), history %s", c, (int)r, h.c_str());
}

void setup() {
  sio::begin();
  bus::begin();                 // every CS high, every CE low, before anything touches SPI
  pinMode(PIN_IR_TX, OUTPUT);   // IR LED off (same pin as the clamped nRF24 #3 CE)
  digitalWrite(PIN_IR_TX, LOW);
  leds::begin();
  display::begin();
  input::begin();
  power::begin();
  det::begin();
  clk::begin();
  sio::out("[fud] boot");
  logReset();
  ui::boot();
  det::setMuteAvailable(post::buzzer());
  sdlog::begin();               // before the radio watcher: mounting needs the bus quiet
  scanner::begin();             // BLE + Wi-Fi on core 0, after POST so it cannot disturb it
  rfwatch::begin();             // nRF24s + CC1101 on core 0, only the radios POST found
  det::armBootGrace();
  sio::out("[fud] READY");
}

void loop() {
  input::poll();
  power::poll();
  devtools::poll();
  if (det::drain()) ui::dirty();
  uint32_t now = millis();
  static uint32_t lastHk = 0, lastReshow = 0;
  if (now - lastHk > 10000) { lastHk = now; det::housekeep(); }
  // Acknowledged attacker still around: Zuse shows it again every ~7 min (FUD).
  if (!det::alarmActive() && det::ackedAttackPresent() && now - lastReshow > 7UL * 60 * 1000) {
    lastReshow = now;
    zuse::react("alarm-attack", 2000);
  }
  ledstatus::tick();
  sdlog::tick();
  clk::tick();
  ui::loop();
  delay(2);
}
