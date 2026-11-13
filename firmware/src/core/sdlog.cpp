#include "sdlog.h"
#include <Preferences.h>
#include <SD.h>
#include <driver/gpio.h>
#include "../board/bus.h"
#include "../board/pins.h"
#include "../board/serial_io.h"
#include "clock.h"
#include "detections.h"
#include "rfwatch.h"

namespace sdlog {

static const char* DIR = "/.fuddeck";
static const char* HEADER = "timestamp,category,label,name,mac,rssi,conf,radio";
static const int QMAX = 24;

static Preferences prefs;
static bool on = true;
static bool mounted = false;
static char path[32] = "";
static uint32_t written = 0;
static uint16_t bootNo = 0;
static char queue[QMAX][120];
static int qn = 0;
static uint32_t lastFlush = 0, lastCheck = 0;
static int lastCd = -1;

bool enabled() { return on; }
bool present() { return mounted; }
const char* fileName() { return mounted ? path : ""; }
uint32_t linesWritten() { return written; }

void setEnabled(bool v) {
  on = v;
  prefs.putBool("on", v);
}

static bool cardInserted() { return digitalRead(PIN_SD_CD) == 0; }   // measured: 0 = in

// FUD-X3: a dated file per day once the clock is trusted, otherwise one file per boot.
static void choosePath() {
  char ds[12];
  if (clk::dateStamp(ds, sizeof(ds))) snprintf(path, sizeof(path), "%s/%s.csv", DIR, ds);
  else snprintf(path, sizeof(path), "%s/boot%04u.csv", DIR, bootNo);
}

static void unmount() {
  bus::lock();
  SD.end();
  bus::sdRelease();
  bus::unlock();
  mounted = false;
}

static bool mount() {
  bus::lock();
  bus::deselectAll();
  bool ok = SD.begin(PIN_SD_CS, SPI, SD_HZ); bus::setDrive();
  if (!ok) {   // this card often needs a second try after any unmount: release and retry
    SD.end();
    bus::sdRelease();
    delay(50);
    ok = SD.begin(PIN_SD_CS, SPI, SD_HZ); bus::setDrive();
  }
  if (ok) {
    if (!SD.exists(DIR)) SD.mkdir(DIR);
    choosePath();   // the header is written by the first flush into an empty file
  }
  if (!ok) { SD.end(); bus::sdRelease(); }
  else bus::sdReleaseQuick();
  bus::unlock();
  mounted = ok;
  sio::out("[sdlog] %s %s", ok ? "logging to" : "card will not mount", ok ? path : "");
  return ok;
}

static void flush() {
  if (!qn || !mounted) return;
  bus::lock();
  choosePath();   // clock just set, or midnight: move to the right file
  // One open per flush. A new (empty) file gets its header from the same handle: a second
  // open right after the first is exactly what this card's flaky first-open trips on.
  File f = SD.open(path, FILE_APPEND);
  bool ok = (bool)f;
  if (ok) {
    if (f.size() == 0) f.println(HEADER);
    for (int i = 0; i < qn; i++) f.println(queue[i]);
    f.close();
    written += qn;
  }
  bus::sdReleaseQuick();
  bus::unlock();
  if (!ok) {
    // Card gone or hiccuping. Remount once right away (this card is intermittent); if that
    // fails, pause and keep the lines queued for the next attempt instead of dropping them.
    sio::out("[sdlog] write failed, remounting");
    unmount();
    static bool retrying = false;   // one immediate retry only, never a loop
    if (!retrying && cardInserted() && mount()) { retrying = true; flush(); retrying = false; return; }
    sio::out("[sdlog] paused, %d lines kept", qn);
    return;
  }
  qn = 0;
}

static void onFresh(const det::Det& d) {
  if (!on || qn >= QMAX) return;   // full queue: drop rather than block the UI
  char name[33];
  // CSV safety: commas and quotes in device names would break columns.
  int j = 0;
  for (int i = 0; d.name[i] && j < 32; i++) name[j++] = (d.name[i] == ',' || d.name[i] == '"') ? ' ' : d.name[i];
  name[j] = 0;
  char ts[24];
  if (clk::trusted()) clk::formatDateTime(ts, sizeof(ts));
  else snprintf(ts, sizeof(ts), "+%lus", (unsigned long)(millis() / 1000));
  snprintf(queue[qn++], sizeof(queue[0]), "%s,%s,%s,%s,%02X:%02X:%02X:%02X:%02X:%02X,%d,%c,%c",
           ts, det::CAT_NAME[d.cat], d.label, name, d.key[0], d.key[1],
           d.key[2], d.key[3], d.key[4], d.key[5], d.rssi, d.conf, d.src);
}

void tail(int lines) {
  if (!mounted) { sio::out("[sdlog] no card"); return; }
  flush();
  bus::lock();
  File f = SD.open(path, FILE_READ);
  if (f) {
    size_t sz = f.size();
    f.seek(sz > (size_t)lines * 90 ? sz - lines * 90 : 0);
    sio::setReliable(true);   // a burst of lines: wait for every byte
    sio::out("[sdlog] %s, %u bytes, %lu lines this boot:", path, (unsigned)sz, (unsigned long)written);
    String l;
    while (f.available()) {
      l = f.readStringUntil('\n');
      sio::out("  %s", l.c_str());
    }
    sio::setReliable(false);
    f.close();
  } else sio::out("[sdlog] cannot open %s", path);
  bus::sdReleaseQuick();
  bus::unlock();
}

void head(const char* file, int lines) {
  if (!mounted) { sio::out("[sdlog] no card"); return; }
  char p[48];
  snprintf(p, sizeof(p), "%s/%s", DIR, file);
  bus::lock();
  File f = SD.open(p, FILE_READ);
  sio::setReliable(true);
  if (f) {
    sio::out("[sdlog] %s, %u bytes:", p, (unsigned)f.size());
    for (int i = 0; i < lines && f.available(); i++) sio::out("  %s", f.readStringUntil('\n').c_str());
    f.close();
  } else sio::out("[sdlog] cannot open %s", p);
  sio::setReliable(false);
  bus::sdReleaseQuick();
  bus::unlock();
}

bool removeFile(const char* file) {
  if (!mounted) return false;
  char p[48];
  snprintf(p, sizeof(p), "%s/%s", DIR, file);
  bus::lock();
  bool ok = SD.remove(p);
  bus::sdReleaseQuick();
  bus::unlock();
  sio::out("[sdlog] remove %s: %s", p, ok ? "ok" : "failed");
  return ok;
}

bool formatIfUnreadable() {
  if (mounted) { sio::out("[sdlog] card already mounts, not formatting"); return false; }
  if (!cardInserted()) { sio::out("[sdlog] no card"); return false; }
  sio::out("[sdlog] formatting (only if no readable FAT filesystem)... this takes a while");
  bus::lock();
  bus::deselectAll();
  // format_if_empty = true: the library runs mkfs only when mount finds no filesystem.
  bool ok = SD.begin(PIN_SD_CS, SPI, SD_HZ, "/sd", 5, true); bus::setDrive();
  if (ok) sio::out("[sdlog] card mounts now: %lluMB, type %d", SD.cardSize() / (1024 * 1024), (int)SD.cardType());
  else sio::out("[sdlog] still will not mount: card not answering, or the format itself failed (format it on a computer)");
  SD.end();
  bus::sdRelease();
  bus::unlock();
  if (ok) mount();
  return ok;
}

// dev: mount reliability and throughput at several SPI clocks. Holds the bus throughout
// (radios pause for a few seconds). Ends by remounting the logger at SD_HZ.
static volatile uint32_t g21Falls = 0;
static void IRAM_ATTR onG21() { g21Falls++; }

// Pad drive for SCK/MOSI during the bench: -1 = leave default, 0-3 = GPIO_DRIVE_CAP_n.
static int benchDrive = -1;
static void applyDrive() {
  if (benchDrive < 0) return;
  gpio_set_drive_capability((gpio_num_t)PIN_SPI_SCK, (gpio_drive_cap_t)benchDrive);
  gpio_set_drive_capability((gpio_num_t)PIN_SPI_MOSI, (gpio_drive_cap_t)benchDrive);
}
static bool benchBegin(uint32_t hz) {
  applyDrive();
  bool ok = SD.begin(PIN_SD_CS, SPI, hz);
  applyDrive();
  return ok;
}

void bench(int mhz, int rounds, const char* quiet) {
  benchDrive = -1;
  if (!strncmp(quiet, "drive", 5)) { benchDrive = quiet[5] - '0'; quiet = ""; }
  static const uint32_t ALL[] = {1000000, 4000000, 8000000, 10000000, 20000000};
  uint32_t one[1] = {(uint32_t)mhz * 1000000UL};
  const uint32_t* HZ = mhz > 0 ? one : ALL;
  int nHz = mhz > 0 ? 1 : 5;
  if (rounds < 1) rounds = 1;
  static const int KB = 32, MOUNTS = 5;
  static uint8_t buf[512];
  bool was = mounted;
  if (was) unmount();
  bus::lock();
  int passes = 0, total = 0;
  // GPIO21 = nRF #3 CSN = IR receiver output. Each falling edge selects nRF #3, which
  // may then drive MISO. Count them per round to see if failures line up.
  attachInterrupt(PIN_NRF3_CSN, onG21, FALLING);
  if (quiet[0]) { rfwatch::benchQuiet(quiet); sio::out("[sdbench] shield radios: %s", quiet); }
  {
    gpio_drive_cap_t c;
    gpio_get_drive_capability((gpio_num_t)PIN_SPI_SCK, &c);
    sio::out("[sdbench] GPIO21 level %d, SCK drive default %d, bench drive %d", digitalRead(PIN_NRF3_CSN), (int)c, benchDrive);
  }
  for (int r = 0; r < rounds; r++)
  for (int h = 0; h < nHz; h++) {
    uint32_t hz = HZ[h];
    total++;
    int firstTry = 0;
    for (int i = 0; i < MOUNTS; i++) {
      bus::deselectAll();
      if (benchBegin(hz)) firstTry++;
      SD.end();
      bus::sdRelease();
    }
    bus::deselectAll();
    bool ok = benchBegin(hz);
    if (!ok) { SD.end(); bus::sdRelease(); delay(50); ok = benchBegin(hz); }
    if (!ok) {
      sio::out("[sdbench] %2lu MHz  mounts %d/%d  will not mount", (unsigned long)(hz / 1000000), firstTry, MOUNTS);
      SD.end(); bus::sdRelease();
      continue;
    }
    if (!SD.exists(DIR)) SD.mkdir(DIR);
    g21Falls = 0;
    const char* tp = "/.fuddeck/bench.tmp";
    SD.remove(tp);
    uint32_t t0 = millis();
    File f = SD.open(tp, FILE_WRITE);
    size_t put = 0;
    for (int b = 0; f && b < KB * 2; b++) {
      for (int i = 0; i < 512; i++) buf[i] = (uint8_t)(b * 7 + i);
      put += f.write(buf, 512);
    }
    if (f) f.close();
    uint32_t tw = millis() - t0;
    t0 = millis();
    f = SD.open(tp, FILE_READ);
    size_t got = 0; int bad = 0;
    for (int b = 0; f && b < KB * 2; b++) {
      int n = f.read(buf, 512);
      if (n != 512) break;
      got += n;
      for (int i = 0; i < 512; i++) if (buf[i] != (uint8_t)(b * 7 + i)) bad++;
    }
    if (f) f.close();
    uint32_t tr = millis() - t0;
    SD.remove(tp);
    SD.end();
    bus::sdRelease();
    bool good = put == (size_t)KB * 1024 && got == (size_t)KB * 1024 && bad == 0;
    if (good) passes++;
    uint32_t falls = g21Falls;
    sio::out("[sdbench] %2lu MHz  mounts %d/%d  write %lu KB/s  read %lu KB/s  g21 lows %lu  %s",
             (unsigned long)(hz / 1000000), firstTry, MOUNTS,
             (unsigned long)(tw ? KB * 1000UL / tw : 0), (unsigned long)(tr ? KB * 1000UL / tr : 0),
             (unsigned long)falls, good ? "verify ok" : "VERIFY FAILED");
    if (!good) sio::out("[sdbench]   wrote %u read %u bad bytes %d", (unsigned)put, (unsigned)got, bad);
  }
  detachInterrupt(PIN_NRF3_CSN);
  if (quiet[0]) rfwatch::benchQuiet("normal");
  bus::unlock();
  sio::out("[sdbench] passed %d of %d", passes, total);
  if (was || cardInserted()) mount();
}

void begin() {
  prefs.begin("sdlog", false);
  on = prefs.getBool("on", true);
  bootNo = prefs.getUShort("boot", 0) + 1;
  prefs.putUShort("boot", bootNo);
  pinMode(PIN_SD_CD, INPUT_PULLUP);
  det::hooks.fresh = onFresh;
  if (cardInserted()) mount();
}

void tick() {
  uint32_t now = millis();
  if (now - lastCheck > 1000) {
    lastCheck = now;
    int cd = cardInserted();
    if (cd != lastCd) {
      lastCd = cd;
      if (cd && !mounted) mount();
      else if (!cd && mounted) { sio::out("[sdlog] card removed"); unmount(); }
    } else if (cd && !mounted && now % 10000 < 1000) {
      mount();   // inserted but failed before: retry every ~10 s
    }
  }
  if (now - lastFlush > 5000 || qn >= QMAX - 4) {
    lastFlush = now;
    flush();
  }
}

}  // namespace sdlog
