// Wall clock: see clock.h.
#include "clock.h"
#include <Preferences.h>
#include <sys/time.h>
#include <time.h>
#include "../board/serial_io.h"

namespace clk {

// ---- zones, west to east. POSIX TZ rules (public tzdata facts). Saved by name, not index,
// so the list can change without moving anyone's zone.
struct Zone { const char* name; const char* rule; };
static const Zone ZONES[] = {
  {"HAWAII", "HST10"},
  {"ALASKA", "AKST9AKDT,M3.2.0,M11.1.0"},
  {"US PACIFIC", "PST8PDT,M3.2.0,M11.1.0"},
  {"ARIZONA", "MST7"},
  {"US MOUNTAIN", "MST7MDT,M3.2.0,M11.1.0"},
  {"MEXICO CITY", "CST6"},
  {"US CENTRAL", "CST6CDT,M3.2.0,M11.1.0"},
  {"BOGOTA", "<-05>5"},
  {"US EASTERN", "EST5EDT,M3.2.0,M11.1.0"},
  {"SANTIAGO", "<-04>4<-03>,M9.1.6/24,M4.1.6/24"},
  {"ATLANTIC", "AST4ADT,M3.2.0,M11.1.0"},
  {"NEWFOUNDLAND", "NST3:30NDT,M3.2.0,M11.1.0"},
  {"SAO PAULO", "<-03>3"},
  {"BUENOS AIRES", "<-03>3"},
  {"UTC", "UTC0"},
  {"LONDON", "GMT0BST,M3.5.0/1,M10.5.0"},
  {"LAGOS", "WAT-1"},
  {"CENTRAL EU", "CET-1CEST,M3.5.0,M10.5.0/3"},
  {"JOHANNESBURG", "SAST-2"},
  {"EASTERN EU", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
  {"JERUSALEM", "IST-2IDT,M3.4.4/26,M10.5.0"},
  {"ISTANBUL", "<+03>-3"},
  {"MOSCOW", "MSK-3"},
  {"NAIROBI", "EAT-3"},
  {"DUBAI", "<+04>-4"},
  {"KARACHI", "PKT-5"},
  {"INDIA", "IST-5:30"},
  {"DHAKA", "<+06>-6"},
  {"BANGKOK", "<+07>-7"},
  {"SINGAPORE", "<+08>-8"},
  {"CHINA", "CST-8"},
  {"PERTH", "AWST-8"},
  {"KOREA", "KST-9"},
  {"JAPAN", "JST-9"},
  {"DARWIN", "ACST-9:30"},
  {"ADELAIDE", "ACST-9:30ACDT,M10.1.0,M4.1.0/3"},
  {"BRISBANE", "AEST-10"},
  {"SYDNEY", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
  {"AUCKLAND", "NZST-12NZDT,M9.5.0,M4.1.0/3"},
};
static const uint8_t ZONES_N = sizeof(ZONES) / sizeof(ZONES[0]);
static const uint8_t ZONE_DEFAULT = 2;   // US PACIFIC until told otherwise

// ---- state

enum class Source : uint8_t { None, Checkpoint, Person };   // where the time came from
static Source source = Source::None;
static Preferences nvs;
static uint8_t zoneIdx = ZONE_DEFAULT;
static uint32_t nextCheckpoint = 0;                          // millis

static const uint32_t FLOOR = 1735689600UL;                  // 2025-01-01: anything earlier is unset
static const uint32_t CHECKPOINT_MS = 10UL * 60 * 1000;      // how often the time is saved to flash

static uint32_t epochNow() { return (uint32_t)time(nullptr); }
static void setSystem(uint32_t e) {
  timeval tv = {(time_t)e, 0};
  settimeofday(&tv, nullptr);
}
static void saveCheckpoint() {
  nvs.putUInt("ckpt", epochNow());
  nextCheckpoint = millis() + CHECKPOINT_MS;
}
static void useZone() {
  setenv("TZ", ZONES[zoneIdx].rule, 1);
  tzset();
}
static bool toLocal(tm& t) {
  if (!isSet()) return false;
  time_t e = epochNow();
  localtime_r(&e, &t);
  return true;
}

// ---- zones

uint8_t zone() { return zoneIdx; }
uint8_t zoneCount() { return ZONES_N; }
const char* zoneName(uint8_t i) { return ZONES[i < ZONES_N ? i : ZONE_DEFAULT].name; }
uint8_t zoneStep(uint8_t i, int dir) { return (i + ZONES_N + (dir < 0 ? -1 : 1)) % ZONES_N; }
void setZone(uint8_t i) {
  zoneIdx = i < ZONES_N ? i : ZONE_DEFAULT;
  nvs.putString("zname", ZONES[zoneIdx].name);
  useZone();
}

// ---- time

bool isSet() { return epochNow() >= FLOOR; }
bool trusted() { return isSet() && source == Source::Person; }
bool guessed() { return isSet() && source == Source::Checkpoint; }
uint32_t nowEpoch() { return isSet() ? epochNow() : 0; }

bool setEpoch(uint32_t e) {
  if (e < FLOOR) return false;
  setSystem(e);
  source = Source::Person;
  nvs.putUChar("src", (uint8_t)source);
  saveCheckpoint();
  char b[24];
  formatDateTime(b, sizeof(b));
  sio::out("[clock] set: %s %s", b, ZONES[zoneIdx].name);
  return true;
}

bool setLocal(int y, int mo, int d, int h, int mi) {
  tm t = {};
  t.tm_year = y - 1900;
  t.tm_mon = mo - 1;
  t.tm_mday = d;
  t.tm_hour = h;
  t.tm_min = mi;
  t.tm_isdst = -1;   // the zone rule decides daylight saving
  time_t e = mktime(&t);
  return e > 0 && setEpoch((uint32_t)e);
}

void begin() {
  nvs.begin("clock", false);
  String zn = nvs.getString("zname", "");
  for (uint8_t i = 0; i < ZONES_N; i++)
    if (zn == ZONES[i].name) zoneIdx = i;
  useZone();
  if (isSet()) {
    // A soft reset keeps the system time; it is as good as it was before.
    source = (Source)nvs.getUChar("src", (uint8_t)Source::Checkpoint);
    sio::out("[clock] kept across reset (%s)", trusted() ? "trusted" : "guessed");
    return;
  }
  // Power was cut: resume from the last checkpoint. Never earlier than it, but behind by
  // however long the power was off, so it is only a guess until someone sets it.
  uint32_t ck = nvs.getUInt("ckpt", 0);
  if (ck < FLOOR) return;
  setSystem(ck);
  source = Source::Checkpoint;
  nvs.putUChar("src", (uint8_t)source);
  sio::out("[clock] resumed from checkpoint (guessed)");
}

void tick() {
  if (isSet() && (int32_t)(millis() - nextCheckpoint) >= 0) saveCheckpoint();
}

void formatTime(char* out, size_t n) {
  tm t;
  if (toLocal(t)) snprintf(out, n, "%02d:%02d", t.tm_hour, t.tm_min);
  else snprintf(out, n, "--:--");
}

void formatDateTime(char* out, size_t n) {
  tm t;
  if (toLocal(t))
    snprintf(out, n, "%04d-%02d-%02d %02d:%02d:%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour,
             t.tm_min, t.tm_sec);
  else snprintf(out, n, "not set");
}

bool dateStamp(char* out, size_t n) {
  tm t;
  if (!trusted() || !toLocal(t)) return false;
  snprintf(out, n, "%04d%02d%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
  return true;
}

void localFields(int& y, int& mo, int& d, int& h, int& mi) {
  tm t;
  if (!toLocal(t)) { y = 2026; mo = 1; d = 1; h = 12; mi = 0; return; }
  y = t.tm_year + 1900;
  mo = t.tm_mon + 1;
  d = t.tm_mday;
  h = t.tm_hour;
  mi = t.tm_min;
}

}  // namespace clk
