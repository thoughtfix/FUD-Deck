// Detection store and alert logic. Portable: no display, no radios, no pins.
// Ported from FUD-Firmware (thoughtfix, GPL-3.0): tiers, ack, ignore, snooze, eviction.
//
// Tiers (same as FUD):
//   active + passes filter     ALARM. Persistent alert until acknowledged. Survives mute
//                              visually; mute only silences the noise (LEDs here).
//   passive + passes filter    one short alert (face + LED), then logged.
//   tracker                    logged silently, with a stuck-with-you timer.
//   below the alert filter     counted and logged, no alert.
#pragma once
#include <Arduino.h>

namespace det {

enum Cat : uint8_t { NONE = 0, TRACKER, CAMERA, DRONE, SUBGHZ, HACKDEV, ATTACK, CAT_COUNT };
extern const char* const CAT_TAG[CAT_COUNT];    // "TRK" ...
extern const char* const CAT_NAME[CAT_COUNT];   // "TRACKERS" ...

// Which radio saw it. Drives the LED language: B=BLE, W=Wi-Fi, S=sub-GHz, N=nRF24 2.4 GHz.
struct Det {
  bool used = false;
  uint8_t key[6] = {0};
  Cat cat = NONE;
  char label[16] = {0};
  char name[32] = {0};
  uint32_t firstSeen = 0, lastSeen = 0;
  uint16_t hits = 0;
  int8_t rssi = 0;
  char conf = 'M';      // signature confidence: H, M, L
  char src = '-';
  bool acked = false, alarm = false, ignored = false, escalated = false, ssidBased = false, active = false;
  // AirTags hop MAC addresses near their owner, so they are shown as one bundled row.
  // Each address still has its own stuck-with-you timer. One that trips (a separated
  // AirTag keeps its address for hours) breaks out of the bundle as its own row.
  bool bundle = false;
  // A device that drops out and comes back keeps its history (FUD-X3 logs each return too).
  uint32_t since = 0;     // millis of the first sighting this boot
  uint16_t visits = 0;    // times it has appeared this boot
  uint16_t place = 0;     // scanner place number when it first appeared (trackers)
};

// list() filters beyond the categories, and the synthetic row for the bundle.
constexpr int FILTER_BUNDLE = 100;   // every bundled AirTag
constexpr int ROW_BUNDLE = -2;       // list(TRACKER) returns this index for the bundle row

constexpr int MAX = 128;

// Side effects the UI wants to know about. Set by the app, called on the UI core.
struct Hooks {
  void (*passive)(const Det& d) = nullptr;     // one-shot alert (face + LED)
  void (*alarm)(const Det& d) = nullptr;       // new active threat
  void (*follow)(const Det& d) = nullptr;      // a tracker has been with you a long time
  void (*fresh)(const Det& d) = nullptr;       // every new, not-ignored detection (SD log)
};
extern Hooks hooks;

void begin();                                  // queue + ignore lists from NVS
// Thread safe. Radio tasks call this from any core.
void enq(const uint8_t key[6], Cat c, const char* label, const char* name, int8_t rssi, char src,
         bool ssid = false, bool active = false, char conf = 'M', bool bundle = false);
void setPlace(uint16_t place, bool known);     // scanner, any core: Wi-Fi says we moved
void armBootGrace();                           // scanners up: alerts from here + 10 s are boot alerts
bool drain();                                  // UI core: ingest queued events. true = something changed
void housekeep();                              // UI core, every ~10 s

Det& at(int i);
bool alarmActive();
int activeAttackIdx();
bool ackedAttackPresent();
int list(int filter, int* idxs);               // filter -1 = active log (no trackers, no acked)
int trackerCount();                            // individual trackers + 1 for the AirTag bundle
int bundleCount();                             // AirTags currently inside the bundle
uint32_t count(Cat c);                          // seen this session, never goes down
int liveCount(Cat c);                          // what the group lists now (trackers: trackerCount())
bool hot(Cat c);                                // not acked or ignored: active until acked, passive for a minute
uint32_t lastDetectionMs();

void ack(int i);
void ackAll();
void ignore(int i);                            // persistent (by SSID for Wi-Fi entries)
void snoozeAll();                              // baseline: everything known goes quiet, new still alerts
void clearAll();                               // log and counters. Keeps mute, filter, ignores
void clearIgnores();
int ignoreCount();

bool muted();                                  // false when no buzzer is fitted
void setMuted(bool m);
void setMuteAvailable(bool yes);               // POST: buzzer found or not
uint8_t alertFilter();                         // 0 ALL, 1 MED, 2 HIGH
void setAlertFilter(uint8_t f);
const char* alertFilterName(uint8_t f);
bool isTrackable(const Det& d);
}
