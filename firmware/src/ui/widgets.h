// Shared look and the few widgets every screen uses.
#pragma once
#include <Arduino.h>
#include "../board/display.h"

namespace theme {
constexpr uint16_t BG = 0x0000;
constexpr uint16_t FG = 0xFFE0;      // FUD amber
constexpr uint16_t DIM = 0x8400;     // dim amber
constexpr uint16_t ALERT = 0xF800;
constexpr uint16_t GOOD = 0x07E0;
constexpr uint16_t WARN = 0xFD20;
constexpr uint16_t INFO = 0x07FF;
constexpr int W = 240, H = 320;
constexpr int HEADER_H = 28;
constexpr int FOOTER_Y = 304;
}

namespace widgets {
// Header bar. showBack draws a touch BACK button at the left (every screen but Home).
void header(const char* title, bool showBack, bool showHamburger = false);
void headerBattery();                     // refresh just the battery corner
bool hitBack(int x, int y);
bool hitHamburger(int x, int y);      // Home's settings button
// Size-1 text wrapped at spaces to w px, from (x, y). Returns the y below the last line.
int drawWrapped(const char* s, int x, int y, int w, uint16_t color, uint16_t bg = theme::BG);
void footer(const char* hint);            // one line of help at the bottom
void footerFlash(const char* msg);        // a message in the footer; footerRestore() puts the hint back
void footerRestore();
void clearBody();

// A scrolling list of rows that works with the D-pad and with taps.
struct ListView {
  int y0 = theme::HEADER_H + 6;
  int rowH = 30;
  int rows = 8;
  int sel = 0, top = 0;
  int lastSel = -1, lastTop = -1;
  // Fills buf with the row text. May set *color for that row.
  void (*label)(int i, char* buf, size_t len, uint16_t* color) = nullptr;
};
void listDraw(ListView& lv, int n, bool full);
void listMove(ListView& lv, int n, int delta);
int listHit(ListView& lv, int n, int x, int y);   // row index or -1
}
