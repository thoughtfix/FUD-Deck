// Wall-clock time. The idea of a clock that is honest about being set, trusted or guessed
// comes from SquachWatch-CYD (Talking Sasquach); this code and its zone table are our own.
//
// Honest about not knowing: until set, everything falls back to time since boot.
// How it gets set: by hand on the SET CLOCK screen now, by GPS later (once per boot on fix).
// Survives a soft reset (the ESP32 keeps system time). After a power cut it restarts from a
// checkpoint saved to flash every 10 minutes: set, but only *guessed* (behind by however long
// the power was off). A real set replaces the guess.
#pragma once
#include <Arduino.h>

namespace clk {
void begin();
void tick();                          // UI loop: saves the checkpoint when due
bool isSet();
bool trusted();                       // set by a person or GPS this power-up
bool guessed();                       // resumed from the flash checkpoint after a power cut
uint32_t nowEpoch();                  // 0 when unset
bool setEpoch(uint32_t epoch);        // rejects anything before 2025
bool setLocal(int y, int mo, int d, int h, int mi);   // from the SET CLOCK screen

void formatTime(char* out, size_t n);       // "21:03" or "--:--"
void formatDateTime(char* out, size_t n);   // "2026-11-10 21:03:12" or "not set"
bool dateStamp(char* out, size_t n);        // "20261110", false when not trusted
void localFields(int& y, int& mo, int& d, int& h, int& mi);   // now, or a default when unset

uint8_t zone();
uint8_t zoneCount();
const char* zoneName(uint8_t i);
uint8_t zoneStep(uint8_t i, int dir);       // west to east order, wraps
void setZone(uint8_t i);                    // saved in NVS
}
