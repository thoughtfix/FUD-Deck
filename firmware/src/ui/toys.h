// The Toys: on-demand instruments. Each one says what radios it needs; the background BLE /
// Wi-Fi scanner is paused (stacks shut down, ~106 KB back) when a toy does not need them.
#pragma once
#include "ui.h"

namespace toys {
// Waterfall: live scrolling spectrum, 2.4 GHz (both nRF24s) or 415-450 MHz (CC1101).
void wfEnter(); void wfDraw(bool full); void wfKey(Key k); void wfTap(int x, int y); void wfTick(); void wfLeave();
// Traffic: how busy 2.4 GHz is over time, low half vs high half.
void trEnter(); void trDraw(bool full); void trKey(Key k); void trTap(int x, int y); void trTick(); void trLeave();
// SSID Scan: networks around you (passive), and a detail page that explains each one.
void ssEnter(); void ssDraw(bool full); void ssKey(Key k); void ssTap(int x, int y); void ssTick(); void ssLeave();
void sdEnter(); void sdDraw(bool full); void sdKey(Key k); void sdTap(int x, int y); void sdTick();
// IR Decoder: point a remote at the board, see the protocol, the code, and the pulses.
void irEnter(); void irDraw(bool full); void irKey(Key k); void irTap(int x, int y); void irTick(); void irLeave();
void irFake();   // dev: show a made-up NEC frame
// TV-B-Gone: FUD's curated TV power-off code set through the IR LED. Fires only on request.
void tvEnter(); void tvDraw(bool full); void tvKey(Key k); void tvTap(int x, int y); void tvTick(); void tvLeave();
// Finder: pick a recent tracker / camera / hacker device, then walk hotter or colder.
void fdEnter(); void fdDraw(bool full); void fdKey(Key k); void fdTap(int x, int y); void fdTick();
void flEnter(); void flDraw(bool full); void flKey(Key k); void flTap(int x, int y); void flTick(); void flLeave();
void finderLock(int detIndex);   // from an Item's Find button, or the Finder list
}
