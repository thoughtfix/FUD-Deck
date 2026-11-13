// Shared radio/SD SPI bus (FSPI) and its rules for this board.
#pragma once
#include <Arduino.h>
#include <SPI.h>

namespace bus {
void begin();          // all CS high, all CE low, SPI up on the radio pins
void deselectAll();
// Some SD cards keep driving MISO after CS goes high until they see clocks with CS high.
// On this board that silences every radio. Call after every SD access.
void sdRelease();
// nRF24 #3's CE (GPIO14) is clamped by the IR TX transistor on stock V2 boards.
// Brief readback at minimum drive strength. True only if CE can really go high.
bool nrf3CeWorks();
// On stock V2 boards nRF24 #3 is dead and GPIO21 belongs to the IR receiver (TSOP), which
// drives that line itself. Until POST proves nRF #3 works, never drive GPIO21 as an output.
void setNrf3Usable(bool yes);

// The radio watcher (core 0) and the SD logger (core 1) share this bus. Take the lock
// around every burst of SPI traffic. Keep bursts short so the radios keep sampling.
void lock();
void unlock();
// Cheap release for a card that stays mounted: clocks with every CS high, no SPI restart.
void sdReleaseQuick();
void setDrive();          // SCK/MOSI pad drive 1: drive 2 (default) breaks SD on our shield
}
