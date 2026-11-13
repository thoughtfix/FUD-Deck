#include "ui.h"
#include "../board/leds.h"
#include "../board/post.h"
#include "../board/serial_io.h"
#include "../core/detections.h"
#include "../core/scanner.h"
#include "../core/rfwatch.h"
#include "../core/zuse.h"
#include "widgets.h"

using namespace theme;

namespace ui {

static ScreenId stack[8] = {ScreenId::Home};
static int depth = 1;
static bool needFull = true, needPartial = false;
static uint32_t footerUntil = 0;   // a notify in the footer, put the hint back after this

static const Screen& cur() { return SCREENS[(int)stack[depth - 1]]; }

ScreenId current() { return stack[depth - 1]; }
const char* currentName() { return cur().name; }
void redraw() { needFull = true; }
void dirty() { needPartial = true; }

// Screens that turn the BLE/Wi-Fi scanner off while they are on top: frees ~100 KB of heap
// for them and keeps our own radio out of their readings (it feeds none of their pictures). The UI owns this, so nested
// screens (Settings > Set clock > back) never switch it on early.
static bool quiet(ScreenId s) {
  switch (s) {
    case ScreenId::Settings: case ScreenId::SetClock: case ScreenId::PostReport: case ScreenId::About:
    case ScreenId::TouchCal: case ScreenId::ButtonTest:
    case ScreenId::Waterfall: case ScreenId::Traffic: case ScreenId::TvbGone: case ScreenId::IrDecode:
    case ScreenId::Log: case ScreenId::Bundle: case ScreenId::Item:
      return true;
    default: return false;
  }
}

// The SPI radios (nRF24, CC1101) follow the same idea: a screen keeps only what it shows.
// Waterfall picks nRF24 or CC1101 itself, by band.
static rfwatch::Focus rfFocus(ScreenId s) {
  if (s == ScreenId::Traffic || s == ScreenId::Waterfall) return rfwatch::Focus::Nrf;
  return quiet(s) ? rfwatch::Focus::None : rfwatch::Focus::All;
}

static void entered() {
  scanner::pause(quiet(current()));
  rfwatch::setFocus(rfFocus(current()));
  if (cur().enter) cur().enter();
  needFull = true;
  footerUntil = 0;
  sio::out("[ui] screen %s  heap %lu  low %lu", cur().name, (unsigned long)ESP.getFreeHeap(),
           (unsigned long)ESP.getMinFreeHeap());
}

// leave() runs when a screen is popped (back, home), not when a child is pushed on top of
// it: a toy keeps its radios while you look at one of its detail pages.
static void leaving() { if (cur().leave) cur().leave(); }

void go(ScreenId id) {
  if (depth < 8) stack[depth++] = id;
  else stack[depth - 1] = id;
  entered();
}

void back() {
  leaving();
  if (depth > 1) depth--;
  entered();
}

void home() {
  while (depth > 1) { leaving(); depth--; }
  depth = 1;
  stack[0] = ScreenId::Home;
  entered();
}

void notify(const char* msg) {
  zuse::toast(msg);
  sio::out("[ui] notify %s", msg);
  if (current() == ScreenId::Home) needPartial = true;
  else { widgets::footerFlash(msg); footerUntil = millis() + 3000; }
}

// ---- boot / POST ----

static int postY = 0;

static void drawPostItem(int, const post::Item& it) {
  static const uint16_t col[4] = {GOOD, INFO, WARN, ALERT};
  tft.setTextSize(1);
  tft.setTextColor(col[it.level], BG);
  tft.drawString(post::levelName(it.level), 6, postY);
  tft.setTextColor(FG, BG);
  tft.drawString(it.name, 42, postY);
  tft.setTextColor(DIM, BG);
  tft.drawString(it.detail, 96, postY);
  postY += 14;
}

void boot() {
  tft.fillScreen(BG);
  tft.setTextColor(FG, BG);
  tft.setTextDatum(textdatum_t::top_center);
  tft.setTextSize(3);
  tft.drawString("FUD-DECK", W / 2, 24);
  tft.setTextSize(1);
  tft.setTextColor(DIM, BG);
  tft.drawString("power-on self test", W / 2, 56);
  tft.setTextDatum(textdatum_t::top_left);
  tft.drawFastHLine(0, 72, W, DIM);
  postY = 82;

  // LED check: walk A..D so a dead LED is obvious.
  const uint8_t c[4][3] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 255}};
  for (int i = 0; i < leds::COUNT; i++) {
    leds::set(i, c[i][0], c[i][1], c[i][2]);
    leds::show();
    delay(80);
  }

  post::run(drawPostItem);
  leds::clear();

  int warn = 0, fail = 0;
  for (int i = 0; i < post::count(); i++) {
    if (post::item(i).level == post::WARN) warn++;
    if (post::item(i).level == post::FAIL) fail++;
  }
  tft.setTextDatum(textdatum_t::top_center);
  tft.setTextSize(2);
  tft.setTextColor(fail ? ALERT : warn ? WARN : GOOD, BG);
  char b[32];
  if (fail || warn) snprintf(b, sizeof(b), "%d fail  %d warn", fail, warn);
  else snprintf(b, sizeof(b), "all good");
  tft.drawString(b, W / 2, postY + 10);
  tft.setTextSize(1);
  tft.setTextColor(DIM, BG);
  tft.drawString("full report: Settings > POST report", W / 2, FOOTER_Y);
  tft.setTextDatum(textdatum_t::top_left);
  delay(2000);
  home();
}

// ---- screen sleep ----

static uint8_t awakeLevel = 255;
static uint32_t sleepAfter = 120000, lastInput = 0;
static uint8_t sleepState = 0;   // 0 awake, 1 dimmed, 2 off
static bool lastAlarm = false;

void setBacklight(uint8_t level) { awakeLevel = level; if (sleepState == 0) tft.setBrightness(level); }
void setSleepAfter(uint32_t ms) { sleepAfter = ms; lastInput = millis(); }
bool asleep() { return sleepState != 0; }
void wake() {
  lastInput = millis();
  if (sleepState == 0) return;
  sleepState = 0;
  tft.setBrightness(awakeLevel);
  sio::out("[ui] wake");
}

// Screens you watch without touching: never sleep there.
static bool keepsAwake(ScreenId id) {
  return id == ScreenId::Waterfall || id == ScreenId::Traffic || id == ScreenId::FinderLock ||
         id == ScreenId::IrDecode || id == ScreenId::Attack;
}

static void sleepTick() {
  bool alarm = det::alarmActive();
  if (alarm && !lastAlarm) wake();
  lastAlarm = alarm;
  if (!sleepAfter || alarm || keepsAwake(current())) { lastInput = millis(); return; }
  uint32_t idle = millis() - lastInput;
  if (sleepState == 0 && idle > sleepAfter) {
    sleepState = 1;
    tft.setBrightness(awakeLevel > 40 ? 20 : awakeLevel / 2);
    sio::out("[ui] dim");
  } else if (sleepState == 1 && idle > sleepAfter + 10000) {
    sleepState = 2;
    tft.setBrightness(0);
    sio::out("[ui] sleep");
  }
}

// ---- main loop ----

void loop() {
  Key k;
  while ((k = input::nextKey()) != Key::None) {
    if (sleepState) { wake(); continue; }   // the waking press does nothing else
    lastInput = millis();
    sio::out("[key] %s", input::keyName(k));
    if (k == Key::Back && current() != ScreenId::Home) back();
    else if (cur().key) cur().key(k);
  }
  Tap t;
  while (input::nextTap(t)) {
    if (sleepState) { wake(); continue; }
    lastInput = millis();
    if (current() == ScreenId::TouchCal) {   // every touch is a calibration point, no BACK or swipes
      sio::out("[tap] %d,%d", t.x, t.y);
      cur().tap(t.x, t.y);
      continue;
    }
    if (t.isSwipe()) {
      sio::out("[swipe] %d,%d d%d,%d", t.x, t.y, t.dx, t.dy);
      if (abs(t.dx) > abs(t.dy)) {
        // Swipe right = back, like a phone.
        if (t.dx > 0 && current() != ScreenId::Home) back();
      } else if (cur().key && current() != ScreenId::Home) {   // Home's UP/DOWN open screens
        // Vertical swipe scrolls: finger up moves the list down, a page at a time.
        Key k = t.dy < 0 ? Key::Down : Key::Up;
        for (int i = 0; i < 7; i++) cur().key(k);   // about one page of an 8-row list
      }
      continue;
    }
    sio::out("[tap] %d,%d", t.x, t.y);
    if (current() != ScreenId::Home && widgets::hitBack(t.x, t.y)) back();
    else if (cur().tap) cur().tap(t.x, t.y);
  }
  sleepTick();
  if (cur().tick) cur().tick();
  if (footerUntil && (int32_t)(millis() - footerUntil) >= 0) {
    footerUntil = 0;
    if (current() != ScreenId::Home && !needFull) widgets::footerRestore();
  }
  if (needFull) {
    needFull = needPartial = false;
    cur().draw(true);
  } else if (needPartial) {
    needPartial = false;
    cur().draw(false);
  }
}

}  // namespace ui
