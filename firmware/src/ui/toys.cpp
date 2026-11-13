// The Toys. Each draws into a 4-bit palette sprite (240 wide) so the scrolling happens in RAM
// and one push updates the screen. 4-bit keeps the buffer small enough for the heap's
// largest free block (~35 KB).
#include "toys.h"
#include "../board/serial_io.h"
#include "../core/rfwatch.h"
#include "../core/scanner.h"
#include "../core/detections.h"
#include "widgets.h"
#include "face.h"
#include <IRrecv.h>
#include <IRremoteESP8266.h>
#include <IRsend.h>
#include <IRutils.h>
#include <driver/gpio.h>
#include "../board/pins.h"
#include <algorithm>

using namespace theme;

namespace toys {

// ---------------------------------------------------------------- shared

static lgfx::LGFX_Sprite* spr = nullptr;

// Spectrum heat: black > blue > cyan > green > yellow > red > white.
static const uint8_t HEAT[16][3] = {
  {0, 0, 0},      {0, 0, 70},    {0, 0, 140},   {0, 40, 210},  {0, 110, 255}, {0, 190, 255},
  {0, 255, 200},  {0, 255, 110}, {60, 255, 0},  {160, 255, 0}, {240, 240, 0}, {255, 180, 0},
  {255, 110, 0},  {255, 40, 0},  {255, 0, 90},  {255, 255, 255},
};

static bool sprMake(int w, int h, const uint8_t (*pal)[3], int n) {
  if (!spr) spr = new lgfx::LGFX_Sprite(&tft);
  spr->setColorDepth(4);
  if (!spr->createSprite(w, h)) { sio::out("[toy] sprite %dx%d failed, heap %lu", w, h, (unsigned long)ESP.getMaxAllocHeap()); return false; }
  spr->createPalette();
  for (int i = 0; i < n; i++) spr->setPaletteColor(i, pal[i][0], pal[i][1], pal[i][2]);
  spr->fillScreen(0);
  return true;
}
static void sprFree() {
  if (spr) spr->deleteSprite();
}

static void button(int x, int y, int w, int h, const char* label, bool on) {
  tft.fillRoundRect(x, y, w, h, 5, on ? FG : BG);
  tft.drawRoundRect(x, y, w, h, 5, FG);
  tft.setTextSize(2);
  tft.setTextColor(on ? BG : FG, on ? FG : BG);
  tft.setTextDatum(textdatum_t::middle_center);
  tft.drawString(label, x + w / 2, y + h / 2);
  tft.setTextDatum(textdatum_t::top_left);
}

static void infoLines(int y, const char* a, const char* b) {
  tft.fillRect(0, y, W, 24, BG);
  tft.setTextSize(1);
  tft.setTextColor(FG, BG);
  tft.drawString(a, 6, y + 1);
  tft.setTextColor(DIM, BG);
  tft.drawString(b, 6, y + 13);
}

// What lives at a 2.4 GHz frequency. Education first.
static void about24(int mhz, char* b, size_t n) {
  if (mhz == 2402 || mhz == 2426 || mhz == 2480) { snprintf(b, n, "%d MHz: Bluetooth LE advertising channel", mhz); return; }
  if (mhz > 2483) { snprintf(b, n, "%d MHz: above Wi-Fi. Mostly quiet (some drones, RC)", mhz); return; }
  int best = 1, bd = 99;
  for (int c = 1; c <= 13; c++) { int d = abs(mhz - (2407 + 5 * c)); if (d < bd) { bd = d; best = c; } }
  if (bd <= 11) snprintf(b, n, "%d MHz: Wi-Fi channel %d area (+BT, Zigbee)", mhz, best);
  else snprintf(b, n, "%d MHz: Bluetooth hops here, Wi-Fi edge", mhz);
}
// The CC1101 hears its own 26 MHz crystal: 16 x 26 = 416 MHz, 17 x 26 = 442 MHz.
static bool ccSpur(float mhz) { return fabsf(mhz - 416.0f) < 0.45f || fabsf(mhz - 442.0f) < 0.45f; }

static void about433(float mhz, char* b, size_t n) {
  if (ccSpur(mhz)) snprintf(b, n, "%.0f MHz: its own 26 MHz crystal x%d", mhz, mhz < 430 ? 16 : 17);
  else if (fabsf(mhz - 433.92f) < 0.6f) snprintf(b, n, "%.2f MHz: key fobs, weather sensors, doorbells", mhz);
  else if (mhz >= 445.9f && mhz <= 446.3f) snprintf(b, n, "%.2f MHz: PMR446 walkie-talkies (EU)", mhz);
  else if (mhz < 420) snprintf(b, n, "%.2f MHz: some remotes and alarm sensors", mhz);
  else snprintf(b, n, "%.2f MHz: 70 cm amateur radio band", mhz);
}

// ---------------------------------------------------------------- Waterfall

static const int WF_Y = 64, WF_H = 200, INFO_Y = WF_Y + WF_H + 3;
static int wfBand = 0;                 // 0 = 2.4 GHz, 1 = 433 band
static bool wfFrozen = false, wfOk = false;
static uint32_t wfLast = 0, wfCcSeq = 0;
static uint16_t wfHeat[126];           // smoothed per-channel heat, for "hottest"
static char wfLine1[64], wfLine2[64];

static void wfScale() {
  tft.fillRect(0, 52, W, 11, BG);
  tft.setTextSize(1);
  tft.setTextDatum(textdatum_t::top_center);
  if (wfBand == 0) {
    // Wi-Fi channels 1/6/11 and the BLE advertising channels, on the 2400-2525 MHz axis.
    const struct { int mhz; const char* t; uint16_t c; } m[] = {
      {2412, "1", FG}, {2437, "6", FG}, {2462, "11", FG}, {2402, "b", INFO}, {2426, "b", INFO}, {2480, "b", INFO}};
    for (auto& k : m) {
      int x = (k.mhz - 2400) * W / 126;
      tft.setTextColor(k.c, BG);
      tft.drawString(k.t, x, 53);
    }
    tft.setTextColor(DIM, BG);
    tft.drawString("2400", 14, 53);
    tft.drawString("2525", W - 14, 53);
  } else {
    int x = (int)((433.92f - 415.0f) * W / 35.0f);
    tft.setTextColor(FG, BG);
    tft.drawString("433.92", x, 53);
    tft.setTextColor(DIM, BG);
    tft.drawString("415", 10, 53);
    tft.drawString("450", W - 10, 53);
  }
  tft.setTextDatum(textdatum_t::top_left);
}

static void wfButtons() {
  button(4, 31, 114, 20, "2.4 GHz", wfBand == 0);
  button(122, 31, 114, 20, "433 MHz", wfBand == 1);
}

static void wfSetBand(int b) {
  wfBand = b;
  rfwatch::setCcSweep(b == 1);
  rfwatch::setFocus(b == 1 ? rfwatch::Focus::Cc : rfwatch::Focus::Nrf);   // only the radio on screen
  memset(wfHeat, 0, sizeof(wfHeat));
  if (wfOk) spr->fillScreen(0);
  wfButtons();
  wfScale();
  snprintf(wfLine1, sizeof(wfLine1), "%s", b ? "CC1101 sweeping 415-450 MHz" : "both nRF24s, half the band each");
  snprintf(wfLine2, sizeof(wfLine2), "tap the waterfall for what lives there");
  infoLines(INFO_Y, wfLine1, wfLine2);
}

// The scanner shuts its stacks down on its own task, and can take a couple of seconds if it is
// mid network scan. Toys allocate their sprite only once that has happened (checked from
// tick), or the sprite lands on top of BLE + Wi-Fi and the heap runs nearly dry.
static bool wfPending = false, trPending = false;

static void waitingNote(const char* what) {
  tft.fillRect(0, 100, W, 40, BG);
  tft.setTextSize(1);
  tft.setTextColor(DIM, BG);
  tft.drawString(what, 8, 110);
}

void wfEnter() {
  // BLE/Wi-Fi are off on this screen (ui.cpp quiet list): frees memory, our own BLE stays out
  rfwatch::setFocus(wfBand == 1 ? rfwatch::Focus::Cc : rfwatch::Focus::Nrf);
  wfFrozen = false;
  wfOk = false;
  wfPending = true;
}
void wfDraw(bool full) {
  if (!full) return;
  tft.fillScreen(BG);
  widgets::header("WATERFALL", true);
  widgets::footer("RIGHT band  CENTER freeze  tap: info");
  if (wfPending) { waitingNote("handing the radios over..."); return; }
  if (!wfOk) {
    tft.setTextColor(ALERT, BG);
    tft.drawString("not enough memory for the waterfall", 6, 100);
    return;
  }
  wfSetBand(wfBand);
  spr->pushSprite(0, WF_Y);
}
void wfKey(Key k) {
  if (k == Key::Right) wfSetBand(wfBand ^ 1);
  else if (k == Key::Center) {
    wfFrozen = !wfFrozen;
    widgets::footer(wfFrozen ? "FROZEN  CENTER to run" : "RIGHT band  CENTER freeze  tap: info");
  }
}
void wfTap(int x, int y) {
  if (y >= 30 && y < 52) { wfSetBand(x < W / 2 ? 0 : 1); return; }
  if (y < WF_Y || y >= WF_Y + WF_H) return;
  if (wfBand == 0) about24(2400 + x * 126 / W, wfLine2, sizeof(wfLine2));
  else about433(415.0f + x * 35.0f / W, wfLine2, sizeof(wfLine2));
  infoLines(INFO_Y, wfLine1, wfLine2);
}
void wfTick() {
  if (wfPending && scanner::stacksDown()) {
    wfPending = false;
    wfOk = sprMake(W, WF_H, HEAT, 16);
    ui::redraw();
    return;
  }
  if (!wfOk || millis() - wfLast < 110) return;
  wfLast = millis();
  uint8_t row[W];
  if (wfBand == 0) {
    uint8_t cnt[126];
    int n = 0;
    rfwatch::grabCounts(cnt, n);
    if (n == 0 || wfFrozen) return;
    int hot = 0;
    for (int c = 0; c < 126; c++) {
      wfHeat[c] = (wfHeat[c] * 7 + cnt[c] * 1000 / n) / 8;
      if (wfHeat[c] > wfHeat[hot]) hot = c;
    }
    for (int x = 0; x < W; x++) {
      int c = x * 126 / W;
      int v = cnt[c] ? 2 + cnt[c] * 13 / n : 0;
      row[x] = v > 15 ? 15 : v;
    }
    uint32_t sum = 0;
    for (int c = 0; c < 126; c++) sum += cnt[c];
    int busy = sum * 100 / (126 * n);
    if (wfHeat[hot] > 60) snprintf(wfLine1, sizeof(wfLine1), "busy %d%%  hottest %d MHz", busy, 2400 + hot);
    else snprintf(wfLine1, sizeof(wfLine1), "busy %d%%  quiet band", busy);
  } else {
    int8_t bins[rfwatch::CC_BINS];
    uint32_t seq = rfwatch::ccRow(bins);
    if (seq == wfCcSeq || wfFrozen) return;
    wfCcSeq = seq;
    // Color relative to the noise floor (the row's median), so real bursts stand out.
    int8_t sorted[rfwatch::CC_BINS];
    memcpy(sorted, bins, sizeof(sorted));
    std::sort(sorted, sorted + rfwatch::CC_BINS);
    int floorDbm = sorted[rfwatch::CC_BINS / 2];
    int hot = -1;
    for (int i = 0; i < rfwatch::CC_BINS; i++) {
      if (ccSpur(415.0f + i * 35.0f / rfwatch::CC_BINS)) continue;   // not a real signal
      if (hot < 0 || bins[i] > bins[hot]) hot = i;
    }
    for (int x = 0; x < W; x++) {
      int v = (bins[x * rfwatch::CC_BINS / W] - floorDbm - 2) * 15 / 24;   // floor+2..+26 dB > 0..15
      row[x] = v < 0 ? 0 : v > 15 ? 15 : v;
    }
    if (hot < 0) hot = 0;
    snprintf(wfLine1, sizeof(wfLine1), "floor %d dBm  peak %.2f MHz %d", floorDbm,
             415.0f + hot * 35.0f / rfwatch::CC_BINS, bins[hot]);
  }
  spr->scroll(0, 1);
  for (int x = 0; x < W; x++) spr->drawPixel(x, 0, row[x]);
  spr->pushSprite(0, WF_Y);
  static uint32_t lastInfo = 0;
  if (millis() - lastInfo > 1000) { lastInfo = millis(); infoLines(INFO_Y, wfLine1, wfLine2); }
}
void wfLeave() {
  wfPending = false;
  rfwatch::setCcSweep(false);
  sprFree();
}

// ---------------------------------------------------------------- Traffic

static const int TR_Y = 96, TR_H = 150;
static const uint8_t TRPAL[6][3] = {{0, 0, 0}, {40, 40, 40}, {0, 220, 0}, {255, 170, 0}, {255, 0, 0}, {255, 255, 0}};
static bool trOk = false;
static uint32_t trLast = 0;
static int trPeak = 0, trNow = 0, trLo = 0, trHi = 0, trScale = 20;

static void trNumbers() {
  tft.fillRect(0, 30, W, 64, BG);
  tft.setTextSize(3);
  uint16_t c = trNow >= 50 ? ALERT : trNow >= 20 ? WARN : GOOD;
  tft.setTextColor(c, BG);
  char b[40];
  snprintf(b, sizeof(b), "%d%%", trNow);
  tft.drawString(b, 8, 34);
  tft.setTextSize(1);
  tft.setTextColor(FG, BG);
  snprintf(b, sizeof(b), "peak %d%%", trPeak);
  tft.drawString(b, 90, 36);
  snprintf(b, sizeof(b), "low half  2400-2462  %3d%%", trLo);
  tft.drawString(b, 90, 52);
  snprintf(b, sizeof(b), "high half 2463-2525  %3d%%", trHi);
  tft.drawString(b, 90, 64);
  tft.setTextColor(DIM, BG);
  snprintf(b, sizeof(b), "share of the band in use, scale 0-%d%%", trScale);
  tft.drawString(b, 6, 82);
}

void trEnter() {
  trPeak = 0;
  trScale = 20;
  trOk = false;
  trPending = true;
}
void trDraw(bool full) {
  if (!full) return;
  tft.fillScreen(BG);
  widgets::header("TRAFFIC", true);
  widgets::footer("CENTER: reset peak");
  if (trPending) { waitingNote("handing the radios over..."); return; }
  if (!trOk) { tft.drawString("not enough memory", 6, 100); return; }
  trNumbers();
  tft.setTextSize(1);
  tft.setTextColor(DIM, BG);
  int y = TR_Y + TR_H + 4;
  tft.drawString("Wi-Fi fills the low half. The high", 6, y);
  tft.drawString("half is mostly quiet, so busy there is", 6, y + 11);
  tft.drawString("unusual. Both halves busy for a long", 6, y + 22);
  tft.drawString("time: likely a jammer.", 6, y + 33);
  spr->pushSprite(0, TR_Y);
}
void trKey(Key k) {
  if (k == Key::Center) { trPeak = 0; trScale = 20; if (trOk) spr->fillScreen(0); trNumbers(); }
}
void trTap(int, int y) {
  if (y > HEADER_H) trKey(Key::Center);
}
void trTick() {
  if (trPending && scanner::stacksDown()) {
    trPending = false;
    trOk = sprMake(W, TR_H, TRPAL, 6);
    ui::redraw();
    return;
  }
  if (!trOk || millis() - trLast < 200) return;
  trLast = millis();
  uint8_t cnt[126];
  int n = 0;
  rfwatch::grabCounts(cnt, n);
  if (n == 0) return;
  uint32_t lo = 0, hi = 0;
  for (int c = 0; c < 63; c++) lo += cnt[c];
  for (int c = 63; c < 126; c++) hi += cnt[c];
  trLo = lo * 100 / (63 * n);
  trHi = hi * 100 / (63 * n);
  trNow = (lo + hi) * 100 / (126 * n);
  if (trNow > trPeak) trPeak = trNow;
  // Scale to what is happening: 0-20%, 0-50% or 0-100%. A rescale clears the old trace.
  int sc = trPeak < 20 ? 20 : trPeak < 50 ? 50 : 100;
  if (sc != trScale) { trScale = sc; spr->fillScreen(0); }
  spr->scroll(-1, 0);
  for (int g = 1; g < 4; g++) spr->drawPixel(W - 1, TR_H * g / 4, 1);   // grid at 25/50/75%
  int h = min(trNow, trScale) * (TR_H - 1) / trScale;
  uint8_t col = trNow >= 50 ? 4 : trNow >= 20 ? 3 : 2;
  if (h) spr->drawFastVLine(W - 1, TR_H - h, h, col);
  spr->drawPixel(W - 1, TR_H - 1 - min(trPeak, trScale) * (TR_H - 1) / trScale, 5);   // peak trace
  spr->pushSprite(0, TR_Y);
  trNumbers();
}
void trLeave() {
  trPending = false;
  sprFree();
}

// ---------------------------------------------------------------- SSID Scan

struct Net { char ssid[33]; uint8_t bssid[6]; int8_t rssi; uint8_t ch; uint8_t enc; uint32_t seen; };
static const int NET_MAX = 40;
static Net nets[NET_MAX];
static int netN = 0;
static widgets::ListView ssLv;
static bool ssWifiUp = false;
static uint32_t ssScans = 0;
static int ssPick = -1;
static uint8_t ssPickBssid[6];

static const char* encName(uint8_t e) {
  static const char* n[] = {"OPEN", "WEP", "WPA", "WPA2", "WPA/WPA2", "WPA2-ENT", "WPA3", "WPA2/WPA3", "WAPI", "OWE", "WPA3-ENT"};
  return e < sizeof(n) / sizeof(n[0]) ? n[e] : "other";
}
static const char* encAbout(uint8_t e) {
  switch (e) {
    case 0: return "OPEN: anyone can join, and traffic is not encrypted over the air. Fine for a captive "
                   "portal, risky for anything private. Use a VPN.";
    case 1: return "WEP: broken since 2001. It can be cracked in minutes. Treat it as open.";
    case 2: return "WPA (the first one): old and weak. Better than WEP, worse than WPA2.";
    case 3: case 4: return "WPA2: the common standard. Safe with a strong password. Weak passwords can be "
                          "guessed offline from a captured handshake.";
    case 5: case 10: return "Enterprise: each person logs in with their own account. Common at work and "
                            "school.";
    case 6: case 7: return "WPA3: the newest. Resists offline password guessing. WPA2/WPA3 means both are "
                           "offered for older devices.";
    case 9: return "OWE (Enhanced Open): no password, but traffic is still encrypted. Better than plain open.";
    default: return "";
  }
}

static void ssLabel(int i, char* b, size_t n, uint16_t* color) {
  const Net& w = nets[i];
  if (w.enc == 0) *color = WARN;   // open networks stand out
  snprintf(b, n, "%-10.10s%3d %4d", w.ssid[0] ? w.ssid : "(hidden)", w.ch, w.rssi);   // fits left of the scroll arrows
}

static uint32_t ssSeqSeen = 0;

// Merge the scanner's latest results (it scans on core 0), age out, strongest first.
static void ssMerge(const scanner::ScanNet* in, int n) {
  uint32_t now = millis();
  for (int i = 0; i < n; i++) {
    int slot = -1;
    for (int k = 0; k < netN; k++) if (!memcmp(nets[k].bssid, in[i].bssid, 6)) { slot = k; break; }
    if (slot < 0) {
      if (netN < NET_MAX) slot = netN++;
      else {
        slot = 0;
        for (int k = 1; k < netN; k++) if (nets[k].seen < nets[slot].seen) slot = k;
      }
      memcpy(nets[slot].bssid, in[i].bssid, 6);
    }
    Net& w = nets[slot];
    memcpy(w.ssid, in[i].ssid, 33);
    w.rssi = in[i].rssi;
    w.ch = in[i].ch;
    w.enc = in[i].enc;
    w.seen = now;
  }
  int j = 0;
  for (int k = 0; k < netN; k++) if (now - nets[k].seen < 30000) nets[j++] = nets[k];
  netN = j;
  std::sort(nets, nets + netN, [](const Net& a, const Net& b) { return a.rssi > b.rssi; });
  ssScans++;
}

// Shared by the list and the detail page. true = new results arrived.
static bool ssService() {
  static scanner::ScanNet buf[40];
  int n = 0;
  uint32_t seq = scanner::ssidResults(buf, 40, n);
  if (seq == ssSeqSeen) return false;
  ssSeqSeen = seq;
  ssWifiUp = true;
  ssMerge(buf, n);
  return true;
}

void ssEnter() {
  ssLv.label = ssLabel;
  scanner::setSsidMode(true);   // idempotent; the scanner task does the Wi-Fi work
}
void ssDraw(bool full) {
  if (full) {
    tft.fillScreen(BG);
    widgets::header("SSID SCAN", true);
    tft.setTextSize(1);
    tft.setTextColor(DIM, BG);
    tft.drawString("network", 10, HEADER_H + 3);
    tft.drawString("ch", 136, HEADER_H + 3);
    tft.drawString("dBm", 180, HEADER_H + 3);
    widgets::footer("tap a network for details");
  }
  ssLv.y0 = HEADER_H + 14;
  ssLv.rows = 8;
  if (netN == 0) {
    tft.fillRect(0, HEADER_H + 14, W, 250, BG);
    tft.setTextSize(1);
    tft.setTextColor(DIM, BG);
    tft.drawString(ssWifiUp ? "no networks heard" : "listening for beacons...", 10, HEADER_H + 30);
    return;
  }
  widgets::listDraw(ssLv, netN, full);
  char b[40];
  snprintf(b, sizeof(b), "%d networks, %d open   passive scan", netN,
           (int)std::count_if(nets, nets + netN, [](const Net& w) { return w.enc == 0; }));
  tft.fillRect(0, ssLv.y0 + 8 * 30 + 2, W, 12, BG);
  tft.setTextColor(DIM, BG);
  tft.setTextSize(1);
  tft.drawString(b, 10, ssLv.y0 + 8 * 30 + 3);
}
static void ssOpen(int i) {
  ssPick = i;
  memcpy(ssPickBssid, nets[i].bssid, 6);
  ui::go(ScreenId::SsidDetail);
}
void ssKey(Key k) {
  if (k == Key::Up) { widgets::listMove(ssLv, netN, -1); ui::dirty(); }
  else if (k == Key::Down) { widgets::listMove(ssLv, netN, 1); ui::dirty(); }
  else if ((k == Key::Center || k == Key::Right) && netN) ssOpen(ssLv.sel);
}
void ssTap(int x, int y) {
  int i = widgets::listHit(ssLv, netN, x, y);
  if (i >= 0) { ssLv.sel = i; ssOpen(i); }
}
void ssTick() {
  if (ssService()) { ssLv.lastTop = -1; ui::dirty(); }
}
void ssLeave() {
  scanner::setSsidMode(false);
  netN = 0;
  ssScans = 0;
  ssWifiUp = false;
}

// ---- detail

static void rssiWords(int r, char* b, size_t n) {
  const char* w = r >= -50 ? "excellent, very close" : r >= -65 ? "good" : r >= -75 ? "fair" : r >= -85 ? "weak" : "barely there";
  snprintf(b, n, "%d dBm  %s", r, w);
}

void sdEnter() {}
void sdDraw(bool full) {
  int i = -1;
  for (int k = 0; k < netN; k++) if (!memcmp(nets[k].bssid, ssPickBssid, 6)) { i = k; break; }
  if (full) {
    tft.fillScreen(BG);
    widgets::header("NETWORK", true);
    widgets::footer("hold LEFT or tap BACK");
  }
  tft.fillRect(0, HEADER_H + 2, W, 270, BG);
  if (i < 0) {
    tft.setTextSize(1);
    tft.setTextColor(DIM, BG);
    tft.drawString("out of range for 30 s, dropped", 8, HEADER_H + 12);
    return;
  }
  const Net& w = nets[i];
  char b[64];
  tft.setTextSize(2);
  tft.setTextColor(w.enc == 0 ? WARN : FG, BG);
  snprintf(b, sizeof(b), "%.19s", w.ssid[0] ? w.ssid : "(hidden)");
  tft.drawString(b, 8, HEADER_H + 8);
  tft.setTextSize(1);
  tft.setTextColor(FG, BG);
  int y = HEADER_H + 32;
  snprintf(b, sizeof(b), "bssid  %02X:%02X:%02X:%02X:%02X:%02X", w.bssid[0], w.bssid[1], w.bssid[2], w.bssid[3], w.bssid[4], w.bssid[5]);
  tft.drawString(b, 8, y); y += 12;
  snprintf(b, sizeof(b), "chan   %d  (%d MHz)%s", w.ch, w.ch <= 14 ? 2407 + 5 * w.ch : 5000 + 5 * w.ch,
           (w.ch == 1 || w.ch == 6 || w.ch == 11) ? "  one of 1/6/11" : "");
  tft.drawString(b, 8, y); y += 12;
  char r[32];
  rssiWords(w.rssi, r, sizeof(r));
  snprintf(b, sizeof(b), "signal %s", r);
  tft.drawString(b, 8, y); y += 12;
  // signal bar: -95 .. -35 dBm
  int bar = constrain((w.rssi + 95) * (W - 16) / 60, 0, W - 16);
  tft.drawRect(8, y, W - 16, 8, DIM);
  tft.fillRect(9, y + 1, max(0, bar - 2), 6, w.rssi >= -65 ? GOOD : w.rssi >= -80 ? WARN : ALERT);
  y += 14;
  snprintf(b, sizeof(b), "lock   %s", encName(w.enc));
  tft.drawString(b, 8, y); y += 12;
  const char* v = scanner::ouiLabel(w.bssid);
  if (v) { snprintf(b, sizeof(b), "maker  %s (camera/drone vendor list)", v); tft.drawString(b, 8, y); y += 12; }
  if (w.bssid[0] & 0x02) { tft.drawString("addr   locally administered (randomized)", 8, y); y += 12; }
  y += 6;
  tft.setTextColor(INFO, BG);
  tft.drawString("What does this mean?", 8, y);
  y += 13;
  // wrap encAbout
  const char* s = encAbout(w.enc);
  char line[42];
  tft.setTextColor(FG, BG);
  while (*s && y < 290) {
    int take = 0, sp = -1;
    while (s[take] && take < 38) { if (s[take] == ' ') sp = take; take++; }
    if (s[take] && sp > 0) take = sp;
    snprintf(line, sizeof(line), "%.*s", take, s);
    tft.drawString(line, 8, y);
    y += 11;
    s += take;
    while (*s == ' ') s++;
  }
  if (!w.ssid[0] && y < 290) {
    tft.setTextColor(DIM, BG);
    tft.drawString("Hidden: the name is not broadcast. It", 8, y + 4);
    tft.drawString("still shows up, so it hides little.", 8, y + 15);
  }
}
void sdKey(Key) {}
void sdTap(int, int) {}
void sdTick() {
  if (ssService()) ui::dirty();   // live signal while you look
}

// ---------------------------------------------------------------- IR Decoder

static IRrecv* irRx = nullptr;
static decode_results irRes;
struct IrSeen { char proto[16]; uint64_t value; uint16_t bits; uint32_t addr, cmd; bool repeat; };
static IrSeen irHist[4];
static int irHistN = 0;
static uint16_t irRaw[100];
static int irRawN = 0;
static uint32_t irCount = 0;

static const char* irAbout(decode_type_t t) {
  switch (t) {
    case NEC: case NEC_LIKE:
      return "NEC: a 9 ms burst, a 4.5 ms gap, then 32 bits (address, inverted address, command, "
             "inverted command). Bits are spaces: short = 0, long = 1. Holding a button sends short "
             "repeat codes.";
    case SAMSUNG: return "Samsung: NEC's cousin with a 4.5 ms leader. 32 bits, sent once per press.";
    case SONY: return "Sony SIRC: a 2.4 ms leader, then 12, 15 or 20 bits. Here the burst length is the "
                      "bit: short = 0, long = 1. Sent at least three times.";
    case RC5: case RC6: return "Philips RC5/RC6: Manchester code. Each bit is a mark-space pair, and a toggle "
                               "bit flips on every new press.";
    case LG: case LG2: return "LG: 28 bits after a leader, close to NEC timing.";
    case PANASONIC: return "Panasonic (Kaseikyo): 48 bits, with a maker ID in the first 16.";
    case UNKNOWN: return "Unknown protocol: no match in the library, but the pulses below are real. "
                         "Air conditioners often send long, unique frames like this.";
    default: return "A known protocol. The pulses below show its marks (light on) and spaces.";
  }
}

static void irPulses(int y) {
  // Marks (38 kHz carrier on) as tall bars, spaces as gaps, scaled to the screen width.
  tft.fillRect(0, y, W, 34, BG);
  tft.drawFastHLine(4, y + 30, W - 8, DIM);
  if (irRawN < 2) return;
  uint32_t total = 0;
  for (int i = 0; i < irRawN; i++) total += irRaw[i];
  if (!total) return;
  float x = 4;
  for (int i = 0; i < irRawN; i++) {
    float w = (float)irRaw[i] * (W - 8) / total;
    if ((i & 1) == 0) tft.fillRect((int)x, y + 6, max(1, (int)w), 24, FG);   // mark
    x += w;
  }
  tft.setTextSize(1);
  tft.setTextColor(DIM, BG);
}

void irEnter() {
  if (!irRx) irRx = new IRrecv(PIN_IR_RX, 1024, 50, true);
  irRx->enableIRIn();
}
void irDraw(bool full) {
  if (full) {
    tft.fillScreen(BG);
    widgets::header("IR DECODER", true);
    widgets::footer("point a remote at the top edge");
  }
  tft.fillRect(0, HEADER_H + 2, W, 272, BG);
  if (!irHistN) {
    const int zs = 100;   // Zuse glancing up at the receiver
    face::draw("glance-left", (W - zs) / 2, HEADER_H + 14, zs, BG);
    tft.setTextSize(2);
    tft.setTextColor(WARN, BG);
    tft.setTextDatum(textdatum_t::top_center);
    tft.drawString("waiting for a", W / 2, 160);
    tft.drawString("remote...", W / 2, 182);
    tft.setTextSize(1);
    tft.setTextColor(DIM, BG);
    tft.drawString("Any TV, AC or soundbar remote works.", W / 2, 220);
    tft.drawString("The receiver: the black dome on top.", W / 2, 232);
    tft.setTextDatum(textdatum_t::top_left);
    return;
  }
  const IrSeen& s = irHist[0];
  char b[48];
  tft.setTextSize(3);
  tft.setTextColor(FG, BG);
  tft.drawString(s.proto, 8, HEADER_H + 6);
  tft.setTextSize(1);
  int y = HEADER_H + 34;
  snprintf(b, sizeof(b), "%u bits  value 0x%llX%s", s.bits, (unsigned long long)s.value, s.repeat ? "  REPEAT" : "");
  tft.drawString(b, 8, y); y += 12;
  if (s.addr || s.cmd) { snprintf(b, sizeof(b), "address 0x%lX  command 0x%lX", (unsigned long)s.addr, (unsigned long)s.cmd); tft.drawString(b, 8, y); }
  y += 14;
  irPulses(y);
  y += 36;
  tft.setTextColor(INFO, BG);
  tft.drawString("How this remote talks:", 8, y);
  y += 12;
  const char* a = irAbout(irRes.decode_type);
  char line[42];
  tft.setTextColor(FG, BG);
  while (*a && y < 250) {
    int take = 0, sp = -1;
    while (a[take] && take < 38) { if (a[take] == ' ') sp = take; take++; }
    if (a[take] && sp > 0) take = sp;
    snprintf(line, sizeof(line), "%.*s", take, a);
    tft.drawString(line, 8, y);
    y += 11;
    a += take;
    while (*a == ' ') a++;
  }
  tft.setTextColor(DIM, BG);
  y = 256;
  for (int i = 1; i < irHistN && i < 4; i++, y += 11) {
    snprintf(b, sizeof(b), "before: %s 0x%llX", irHist[i].proto, (unsigned long long)irHist[i].value);
    tft.drawString(b, 8, y);
  }
}
// Dev builds: show a made-up NEC frame (address 0x04, command 0x08) without any IR at all.
void irFake() {
  memmove(&irHist[1], &irHist[0], sizeof(IrSeen) * 3);
  IrSeen& s = irHist[0];
  snprintf(s.proto, sizeof(s.proto), "NEC");
  s.value = 0x20DF10EF; s.bits = 32; s.addr = 0x04; s.cmd = 0x08; s.repeat = false;
  if (irHistN < 4) irHistN++;
  irRes.decode_type = NEC;
  irRawN = 0;
  irRaw[irRawN++] = 9000; irRaw[irRawN++] = 4500;
  uint32_t v = 0x20DF10EF;
  for (int i = 31; i >= 0 && irRawN < 98; i--) { irRaw[irRawN++] = 560; irRaw[irRawN++] = ((v >> i) & 1) ? 1690 : 560; }
  irRaw[irRawN++] = 560;
  ui::dirty();
}
void irKey(Key) {}
void irTap(int, int) {}
void irTick() {
  if (!irRx || !irRx->decode(&irRes)) return;
  irCount++;
  memmove(&irHist[1], &irHist[0], sizeof(IrSeen) * 3);
  IrSeen& s = irHist[0];
  snprintf(s.proto, sizeof(s.proto), "%s", typeToString(irRes.decode_type, irRes.repeat).c_str());
  s.value = irRes.value;
  s.bits = irRes.bits;
  s.addr = irRes.address;
  s.cmd = irRes.command;
  s.repeat = irRes.repeat;
  if (irHistN < 4) irHistN++;
  irRawN = 0;
  for (uint16_t i = 1; i < irRes.rawlen && irRawN < 100; i++) irRaw[irRawN++] = irRes.rawbuf[i] * kRawTick;
  sio::out("[ir] %s 0x%llX %u bits", s.proto, (unsigned long long)s.value, s.bits);
  irRx->resume();
  ui::dirty();
}
void irLeave() {
  if (irRx) irRx->disableIRIn();
  pinMode(PIN_IR_RX, INPUT_PULLUP);
}

// ---------------------------------------------------------------- TV-B-Gone

// FUD-Firmware's curated set (thoughtfix, GPL-3.0): facts only, protocol + power code.
enum IrP { P_NEC, P_SONY, P_SAMS, P_RC5, P_RC6, P_PANA, P_LG };
struct IrCode { uint8_t p; uint64_t d; uint16_t bits; const char* brand; };
static const IrCode TVBG[] = {
  {P_SAMS, 0xE0E040BF, 32, "Samsung"},        {P_LG, 0x20DF10EF, 32, "LG / Vizio"},
  {P_NEC, 0x20DF10EF, 32, "LG (NEC)"},        {P_NEC, 0x2FD48B7, 32, "Toshiba / NEC TVs"},
  {P_SONY, 0xA90, 12, "Sony 12-bit"},         {P_SONY, 0xA90, 15, "Sony 15-bit"},
  {P_SONY, 0xA90, 20, "Sony 20-bit"},         {P_PANA, 0x40040100BCBDULL, 48, "Panasonic"},
  {P_RC5, 0x0C, 12, "Philips RC5"},           {P_RC6, 0x0C, 20, "Philips RC6"},
  {P_NEC, 0x57E3E817, 32, "TCL / Hisense"},   {P_NEC, 0x1EE17887, 32, "Sharp / misc"},
};
static const int TVBG_N = sizeof(TVBG) / sizeof(TVBG[0]);
static IRsend* irTx = nullptr;
static bool tvFiring = false;
static int tvIdx = 0;
static uint32_t tvLast = 0;

static void tvSend(const IrCode& c) {
  switch (c.p) {
    case P_NEC: irTx->sendNEC(c.d, c.bits); break;
    case P_SONY: irTx->sendSony(c.d, c.bits, 2); break;
    case P_SAMS: irTx->sendSAMSUNG(c.d, c.bits); break;
    case P_RC5: irTx->sendRC5(c.d, c.bits); break;
    case P_RC6: irTx->sendRC6(c.d, c.bits); break;
    case P_PANA: irTx->sendPanasonic64(c.d, c.bits); break;
    case P_LG: irTx->sendLG(c.d, c.bits); break;
  }
}

void tvEnter() {
  if (!irTx) irTx = new IRsend(PIN_IR_TX);
  tvFiring = false;
  tvIdx = 0;
}
static void tvBody() {
  tft.fillRect(0, HEADER_H + 2, W, 272, BG);
  tft.setTextSize(1);
  tft.setTextColor(FG, BG);
  tft.drawString("Sends TV power codes for the common", 8, HEADER_H + 10);
  tft.drawString("brands, one after another. Point the", 8, HEADER_H + 21);
  tft.drawString("top of the board at the TV.", 8, HEADER_H + 32);
  tft.setTextColor(DIM, BG);
  tft.drawString("The one transmit in FUD-Deck. A tool,", 8, HEADER_H + 48);
  tft.drawString("at worst a prank. It toggles power,", 8, HEADER_H + 59);
  tft.drawString("so a TV that is off may turn ON.", 8, HEADER_H + 70);
  char b[40];
  if (tvFiring || tvIdx) {
    snprintf(b, sizeof(b), "%s %d/%d", tvFiring ? "sending" : "done", tvIdx, TVBG_N);
    tft.setTextSize(2);
    tft.setTextColor(FG, BG);
    tft.drawString(b, 8, 128);
    tft.drawRect(8, 152, W - 16, 14, FG);
    tft.fillRect(9, 153, (W - 18) * tvIdx / TVBG_N, 12, FG);
    if (tvIdx > 0 && tvIdx <= TVBG_N) {
      tft.setTextSize(1);
      tft.drawString(TVBG[tvIdx - 1].brand, 8, 172);
    }
  }
  button(30, 200, 180, 56, tvFiring ? "STOP" : "FIRE", tvFiring);
}
void tvDraw(bool full) {
  if (full) {
    tft.fillScreen(BG);
    widgets::header("TV-B-GONE", true);
    widgets::footer("CENTER or tap: fire / stop");
  }
  tvBody();
}
static void tvToggle() {
  tvFiring = !tvFiring;
  if (tvFiring) {
    tvIdx = 0;
    irTx->begin();
    // IO14 drives the IR transistor's base with no resistor, and the LED has no current limit.
    // Weakest drive strength: ~5 mA of base current still switches the LED fully.
    gpio_set_drive_capability((gpio_num_t)PIN_IR_TX, GPIO_DRIVE_CAP_0);
  } else digitalWrite(PIN_IR_TX, LOW);
  ui::dirty();
}
void tvKey(Key k) { if (k == Key::Center || k == Key::Right) tvToggle(); }
void tvTap(int x, int y) { if (y >= 200 && y < 256) tvToggle(); }
void tvTick() {
  if (!tvFiring || millis() - tvLast < 110) return;   // one code per tick keeps BACK responsive
  tvLast = millis();
  if (tvIdx < TVBG_N) { tvSend(TVBG[tvIdx]); tvIdx++; ui::dirty(); }
  else { tvFiring = false; digitalWrite(PIN_IR_TX, LOW); ui::dirty(); }
}
void tvLeave() {
  tvFiring = false;
  pinMode(PIN_IR_TX, OUTPUT);
  digitalWrite(PIN_IR_TX, LOW);   // IR LED off, no current into Q1's base
}

// ---------------------------------------------------------------- Finder

static int fdIdx[det::MAX];
static int fdN = 0;
static widgets::ListView fdLv;
static uint32_t fdLast = 0;

static void fdBuild() {
  fdN = 0;
  uint32_t now = millis();
  for (int i = 0; i < det::MAX; i++) {
    det::Det& d = det::at(i);
    if (det::isTrackable(d) && d.src == 'B' && !d.ignored && now - d.lastSeen < 5UL * 60 * 1000) fdIdx[fdN++] = i;
  }
  // Fresh ones first, closest first; then the stale ones, most recently heard first.
  uint32_t now2 = millis();
  std::sort(fdIdx, fdIdx + fdN, [now2](int a, int b) {
    const det::Det &x = det::at(a), &y = det::at(b);
    bool fx = now2 - x.lastSeen < 30000, fy = now2 - y.lastSeen < 30000;
    if (fx != fy) return fx;
    return fx ? x.rssi > y.rssi : x.lastSeen > y.lastSeen;
  });
}
// Something not heard for this long cannot be hunted: the signal is too stale to steer by.
static const uint32_t FD_FRESH_MS = 30000;
static uint32_t fdMsgUntil = 0;
static bool fdFresh(const det::Det& d) { return millis() - d.lastSeen < FD_FRESH_MS; }
static void fdLabel(int i, char* b, size_t n, uint16_t* color) {
  det::Det& d = det::at(fdIdx[i]);
  const char* nm = d.name[0] ? d.name : d.label;
  // The last two address bytes tell same-named devices apart (seven "AirTag" rows).
  if (!fdFresh(d)) {   // stale: dim, with its age instead of a signal
    *color = DIM;
    uint32_t s = (millis() - d.lastSeen) / 1000;
    if (s < 100) snprintf(b, n, "%-7.7s %02X%02X %3lus", nm, d.key[4], d.key[5], (unsigned long)s);
    else snprintf(b, n, "%-7.7s %02X%02X %3lum", nm, d.key[4], d.key[5], (unsigned long)(s / 60));
    return;
  }
  if (d.cat == det::HACKDEV) *color = ALERT;
  snprintf(b, n, "%-7.7s %02X%02X %4d", nm, d.key[4], d.key[5], d.rssi);
}
// Pick one: lock on if it is fresh, else say why not (for a few seconds, over the list).
static void fdPick(int i) {
  det::Det& d = det::at(fdIdx[i]);
  if (fdFresh(d)) { finderLock(fdIdx[i]); return; }
  char b[160];
  snprintf(b, sizeof(b), "Last heartbeat %lu seconds ago.\n\nObject out of range, changed MAC, or broadcasts "
           "too slow to locate.", (unsigned long)((millis() - d.lastSeen) / 1000));
  const int y = HEADER_H + 60;
  tft.fillRoundRect(12, y, W - 24, 96, 8, BG);
  tft.drawRoundRect(12, y, W - 24, 96, 8, WARN);
  widgets::drawWrapped(b, 22, y + 12, W - 44, WARN);
  fdMsgUntil = millis() + 4000;
}
void fdEnter() { fdLv.label = fdLabel; fdBuild(); }
void fdDraw(bool full) {
  if (full) {
    tft.fillScreen(BG);
    widgets::header("FINDER", true);
    widgets::footer("pick one to hunt  (dim: quiet 30 s+)");
  }
  if (!fdN) {
    tft.fillRect(0, HEADER_H + 2, W, 270, BG);
    tft.setTextSize(1);
    tft.setTextColor(DIM, BG);
    tft.drawString("Nothing to hunt yet. Trackers, cameras", 8, HEADER_H + 14);
    tft.drawString("and hacker devices seen over Bluetooth", 8, HEADER_H + 25);
    tft.drawString("in the last 5 minutes show up here.", 8, HEADER_H + 36);
    const int zs = 110;   // Zuse looking around for something
    face::draw("glance-right", (W - zs) / 2, HEADER_H + 70, zs, BG);
    return;
  }
  widgets::listDraw(fdLv, fdN, full);
}
void fdKey(Key k) {
  if (k == Key::Up) { widgets::listMove(fdLv, fdN, -1); ui::dirty(); }
  else if (k == Key::Down) { widgets::listMove(fdLv, fdN, 1); ui::dirty(); }
  else if ((k == Key::Center || k == Key::Right) && fdN) fdPick(fdLv.sel);
}
void fdTap(int x, int y) {
  int i = widgets::listHit(fdLv, fdN, x, y);
  if (i >= 0) fdPick(i);
}
void fdTick() {
  if (fdMsgUntil) {   // the "cannot hunt" note stays up, then the list comes back
    if ((int32_t)(millis() - fdMsgUntil) < 0) return;
    fdMsgUntil = 0;
    fdLast = 0;
  }
  if (millis() - fdLast < 2000) return;
  fdLast = millis();
  fdBuild();
  fdLv.lastTop = -1;
  ui::dirty();
}

// ---- locked on

static uint8_t flMac[6];
static char flName[33], flKind[16];
static bool flBundle = false;
static int flSmooth = -100, flBase = -100;
static bool flHave = false;
static uint32_t flLast = 0;

void finderLock(int i) {
  det::Det& d = det::at(i);
  memcpy(flMac, d.key, 6);
  snprintf(flName, sizeof(flName), "%s", d.name[0] ? d.name : d.label);
  snprintf(flKind, sizeof(flKind), "%s", d.label);
  flBundle = d.bundle;
  flSmooth = flBase = d.rssi;
  flHave = false;
  ui::go(ScreenId::FinderLock);
}

static const char* flWords(int r) {
  return r >= -45 ? "RIGHT HERE" : r >= -55 ? "VERY CLOSE" : r >= -65 ? "CLOSE" : r >= -75 ? "NEARBY" : r >= -85 ? "FAR" : "FAINT";
}

void flEnter() { scanner::setFinderTarget(flMac); }
void flDraw(bool full) {
  if (full) {
    tft.fillScreen(BG);
    widgets::header("HUNTING", true);
    tft.setTextSize(2);
    tft.setTextColor(FG, BG);
    char b[24];
    snprintf(b, sizeof(b), "%.19s", flName);
    tft.drawString(b, 8, HEADER_H + 6);
    tft.setTextSize(1);
    tft.setTextColor(DIM, BG);
    snprintf(b, sizeof(b), "%02X:%02X:%02X:%02X:%02X:%02X", flMac[0], flMac[1], flMac[2], flMac[3], flMac[4], flMac[5]);
    tft.drawString(b, 8, HEADER_H + 26);
    int y = 236;
    tft.drawString("Walk, pause, watch the trend. Walls,", 8, y);
    tft.drawString("bodies and pockets change the signal.", 8, y + 11);
    if (flBundle) {
      tft.setTextColor(WARN, BG);
      tft.drawString("AirTags hop addresses near the owner.", 8, y + 26);
      tft.drawString("If it goes quiet, pick it again.", 8, y + 37);
    }
    widgets::footer("tap or CENTER: reset the trend");
  }
  int8_t r;
  uint32_t age;
  bool heard = scanner::finderReading(r, age);
  // Redraw only when something visible changed (no flicker at 4 Hz).
  static int lastSm = 999, lastBase = 999, lastState = -1;
  int state = !heard ? 0 : age > 8000 ? 1 : 2;
  if (!full && state == lastState && flSmooth == lastSm && flBase == lastBase) return;
  lastState = state; lastSm = flSmooth; lastBase = flBase;
  tft.fillRect(0, HEADER_H + 40, W, 160, BG);
  if (!heard || age > 8000) {
    tft.setTextSize(2);
    tft.setTextColor(DIM, BG);
    tft.drawString(heard ? "signal lost" : "listening...", 8, HEADER_H + 70);
    tft.setTextSize(1);
    tft.drawString(heard ? "not heard for 8 s. keep moving." : "waiting for its next advertisement", 8, HEADER_H + 96);
    return;
  }
  char b[24];
  tft.setTextSize(4);
  tft.setTextColor(FG, BG);
  snprintf(b, sizeof(b), "%d", flSmooth);
  tft.drawString(b, 8, HEADER_H + 44);
  tft.setTextSize(1);
  tft.drawString("dBm", 8 + strlen(b) * 24 + 4, HEADER_H + 64);
  tft.setTextSize(3);
  uint16_t c = flSmooth >= -55 ? ALERT : flSmooth >= -70 ? WARN : GOOD;
  tft.setTextColor(c, BG);
  tft.drawString(flWords(flSmooth), 8, HEADER_H + 84);
  // meter: -100 .. -30 dBm
  int m = constrain((flSmooth + 100) * (W - 16) / 70, 0, W - 16);
  tft.drawRect(8, HEADER_H + 116, W - 16, 18, FG);
  tft.fillRect(9, HEADER_H + 117, max(0, m - 2), 16, c);
  int d = flSmooth - flBase;
  tft.setTextSize(2);
  tft.setTextColor(d >= 3 ? ALERT : d <= -3 ? INFO : DIM, BG);
  tft.drawString(d >= 3 ? "HOTTER" : d <= -3 ? "COLDER" : "steady", 8, HEADER_H + 144);
  tft.setTextSize(1);
  tft.setTextColor(DIM, BG);
  snprintf(b, sizeof(b), "%+d dB vs start", d);
  tft.drawString(b, 110, HEADER_H + 150);
}
void flKey(Key k) { if (k == Key::Center) { flBase = flSmooth; ui::dirty(); } }
void flTap(int, int y) { if (y > HEADER_H) { flBase = flSmooth; ui::dirty(); } }
void flTick() {
  if (millis() - flLast < 250) return;
  flLast = millis();
  int8_t r;
  uint32_t age;
  if (scanner::finderReading(r, age) && age < 8000) {
    if (!flHave) { flSmooth = flBase = r; flHave = true; }
    else flSmooth = (flSmooth * 2 + r) / 3;   // FUD's smoothing
  }
  ui::dirty();
}
void flLeave() { scanner::setFinderTarget(nullptr); }

}  // namespace toys
