// SD card detection log, in the style of FUD-X3's Logger (thoughtfix, GPL-3.0).
//
// Every new detection becomes one CSV line:
//   timestamp,category,label,name,mac,rssi,conf,radio
// With the clock trusted (SET CLOCK, later GPS): a file per day, /.fuddeck/20261110.csv,
// with real timestamps. Without: FUD-X3's no-clock fallback, one file per boot
// (/.fuddeck/boot0007.csv) stamped in seconds since boot ("+123s").
//
// Hot-swap safe: no card = no logging and no hang. A pulled card is noticed (card-detect
// pin) and logging resumes when one is inserted. Lines are batched in RAM and written every
// few seconds under the SPI bus lock, so the radios that share the bus keep sampling.
#pragma once
#include <Arduino.h>

namespace sdlog {
void begin();                 // after POST
void tick();                  // UI loop: card detect, periodic flush
bool enabled();
void setEnabled(bool on);     // saved in NVS
bool present();               // card mounted and writable
const char* fileName();       // current log file, "" when none
uint32_t linesWritten();
void tail(int lines);         // dev: print the end of the current file
void head(const char* file, int lines);   // dev: print the start of /.fuddeck/<file>
bool removeFile(const char* file);        // dev: delete /.fuddeck/<file>
// dev: format a card that has NO readable FAT filesystem (exFAT, blank) as FAT32.
// A card that already mounts is never touched. Erases everything on the card.
bool formatIfUnreadable();
void bench(int mhz = 0, int rounds = 1, const char* quiet = "");   // dev: mount/write/read/verify; 0 MHz = 1-20 MHz
}
