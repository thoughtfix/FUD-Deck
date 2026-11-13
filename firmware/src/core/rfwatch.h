// Background watcher for the SPI radios: nRF24 (2 on stock V2, 3 if nRF #3's CE works)
// and the CC1101. All of them listen at the same time. Only reading their results takes
// turns on the shared SPI bus.
//
// 2.4 GHz: the band (nRF channels 0-125 = 2400-2525 MHz) is split into one segment per
// radio. Each step tunes every radio to the same offset in its own segment, so one sweep
// is a snapshot of the whole band at once.
//
// Jammer rule: EVERY segment at least 40% hot in the same sweep, for most sweeps over ~1 s.
// Busy Wi-Fi (channels 1/6/11 reach 2473 MHz) cannot light the top segment, a broadband
// jammer does. The ESP32's own BLE advertising channels (2402, 2426, 2480) are masked
// because our active BLE scan transmits right next to the nRF24 antennas.
//
// 433 MHz: the CC1101 stays in receive and RSSI is sampled every sweep. A few strong samples
// in a short window = something nearby is transmitting (fob, TPMS, remote, Flipper).
#pragma once
#include <Arduino.h>

namespace rfwatch {
void begin();                    // starts the task if POST found any of these radios
void setSaver(bool on);          // battery saver: sweep far less often
int nrfRadios();                 // radios in use
bool ccActive();
// Latest data, for the status line and the toys (Traffic, Waterfall).
int busyPct();                   // % of unmasked 2.4 GHz channels hot in the last sweep
void lastSweep(uint8_t* hot126); // 1 = carrier on that channel in the last sweep
int ccRssi();                    // dBm, last sample
int ccFloor();                   // dBm, slow noise floor estimate
uint32_t sweeps();
int segPct(int i);               // last sweep, % hot in radio i's segment (0 = lowest)

// Waterfall support.
// Per-channel hit counts since the last grab (and how many sweeps that covers), then reset.
void grabCounts(uint8_t* counts126, int& sweepsCovered);
// 433 band sweep: while on, the CC1101 steps 415-450 MHz instead of sitting on 433.92.
// Background 433 burst detection pauses meanwhile. Off: back to 433.92 receive.
void setCcSweep(bool on);
constexpr int CC_BINS = 60;
constexpr uint32_t CC_LO_HZ = 415000000UL, CC_HI_HZ = 450000000UL;
uint32_t ccRow(int8_t* dbm60);   // latest sweep (dBm per bin); returns its sequence number
// Which radios run: a screen keeps only the ones it shows. Off radios are powered down
// (nRF24 PWR_UP=0, CC1101 SPWD); detection from them pauses. Applied by the task.
enum class Focus : uint8_t { All, Nrf, Cc, None };
void setFocus(Focus f);
void benchQuiet(const char* how);   // dev: gdo | ccoff | nrfoff | alloff | normal
}
