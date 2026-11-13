// Background scanner for the ESP32's own radio: BLE, Wi-Fi networks, Wi-Fi deauth monitor.
// Runs as a FreeRTOS task on core 0 so the UI on core 1 never waits on it.
// Classification ported from FUD-Firmware (thoughtfix, GPL-3.0).
//
// Full power (default): BLE and Wi-Fi run TOGETHER through the ESP32's radio coexistence.
// BLE scans continuously (active: Flipper detection needs scan-response names), the Wi-Fi
// deauth monitor hops 1/6/11 alongside, and a passive network scan (beacons only) runs
// every 30 s. The one 2.4 GHz radio is time-sliced in milliseconds, not seconds.
// Battery saver: FUD's rotation, one job at a time with rests.
#pragma once
#include <Arduino.h>

namespace scanner {
void begin();
char activeRadio();          // '*' both (full power), 'B', 'W', or '-' (saver phases)
bool bleActive();
bool wifiActive();
void setSaver(bool on);      // battery saver: FUD-style rotation
void pause(bool on);         // toys that need the radio to themselves
bool paused();
bool stacksDown();
// Finder hunt mode: Wi-Fi quiet, BLE continuous and passive with duplicates, so every
// advertisement from the target updates its signal. nullptr ends the hunt.
void setFinderTarget(const uint8_t* mac);
bool finderReading(int8_t& rssi, uint32_t& ageMs);   // false = never heard since the lock

// SSID mode for the SSID Scan toy: the scanner task (core 0) runs passive network scans and
// publishes them here. Wi-Fi must only ever be started from core 0: starting it from the UI
// core makes the driver set up its interrupt through the tiny ipc0 task, which overflows.
struct ScanNet { char ssid[33]; uint8_t bssid[6]; int8_t rssi; uint8_t ch; uint8_t enc; };
void setSsidMode(bool on);
uint32_t ssidResults(ScanNet* out, int max, int& n);   // returns a sequence number           // paused AND the Wi-Fi/BLE stacks are actually shut down
// Vendor label from the signature OUI table (camera/drone makers), nullptr if none.
const char* ouiLabel(const uint8_t* mac);
uint32_t cycles();           // completed rotations, for the status line
}
