// Screen stack. Each screen draws itself and handles keys and taps.
#pragma once
#include <Arduino.h>
#include "../board/input.h"

enum class ScreenId : uint8_t {
  Home, Menu, Log, Bundle, Item, Attack, Groups, Toys, ToyStub, Waterfall, Traffic, SsidScan, SsidDetail, IrDecode, TvbGone, Finder, FinderLock, Settings, SetClock, PostReport, About, TouchCal, ButtonTest, Pin, Prank, PrankHit, COUNT
};

struct Screen {
  const char* name;
  void (*enter)();                // called when the screen becomes current (also on return)
  void (*draw)(bool full);        // full = repaint everything
  void (*key)(Key k);
  void (*tap)(int x, int y);
  void (*tick)();                 // optional periodic work while current
  void (*leave)();                // optional: called when the screen stops being current
};

namespace ui {
void boot();                      // POST screen for 2 s, then Home
void loop();
void go(ScreenId id);             // push
void back();                      // pop (Home stays)
void home();
void redraw();                    // full repaint of the current screen
void dirty();                     // partial repaint
ScreenId current();
const char* currentName();
void notify(const char* msg);     // short message: Zuse's toast on Home, footer elsewhere
// Screen sleep: dim after `ms` with no input, backlight off 10 s later. 0 = never.
// Any key or tap wakes it and is swallowed. An alarm wakes it. Live toys stay awake.
void setBacklight(uint8_t level); // the awake brightness (Settings)
void setSleepAfter(uint32_t ms);
void wake();
bool asleep();
}

// Defined in screens.cpp
extern const Screen SCREENS[];
