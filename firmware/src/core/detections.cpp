#include "detections.h"
#include <Preferences.h>
#include "../board/serial_io.h"
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

namespace det {

const char* const CAT_TAG[CAT_COUNT] = {"clr", "TRK", "CAM", "DRN", "SUB", "HAK", "ATK"};
const char* const CAT_NAME[CAT_COUNT] = {"", "TRACKERS", "CAMERAS", "DRONES", "SUB-GHZ", "HACK DEV", "ATTACKS"};

Hooks hooks;

static const uint32_t EVICT_MS = 5 * 60 * 1000;
static const uint32_t EVICT_TRK_MS = 30 * 60 * 1000;
static const uint32_t TRK_FOLLOW_MS = 10 * 60 * 1000;
// Place from the Wi-Fi scanner. Unknown (no Wi-Fi heard yet, or too few networks): fall back
// to time alone, as FUD does.
static volatile uint16_t placeNow = 0;
static volatile bool placeKnown = false;
void setPlace(uint16_t p, bool known) { placeNow = p; placeKnown = known; }
static const uint32_t ALARM_STANDDOWN_MS = 60000;

struct Evt {
  uint8_t key[6];
  Cat cat;
  char label[16];
  char name[32];
  int8_t rssi;
  char src;
  bool ssid, active;
  char conf;
  bool bundle;
};

static QueueHandle_t q = nullptr;
static Det dets[MAX];
static uint32_t counts[CAT_COUNT] = {0};
static uint32_t lastDet = 0;
static bool mute = false;
static uint8_t filt = 0;
static Preferences prefs;

static const int MAXIGN = 32, MAXIGNSS = 16;
static uint8_t ignKeys[MAXIGN][6];
static int ignN = 0;
static char ignSsid[MAXIGNSS][33];
static int ignSsidN = 0;

static int confRank(char c) { return c == 'H' ? 2 : (c == 'M' ? 1 : 0); }

void begin() {
  q = xQueueCreate(48, sizeof(Evt));
  prefs.begin("fuddet", false);
  mute = prefs.getBool("mute", false);
  filt = prefs.getUChar("alf", 0);
  if (prefs.isKey("ign")) {
    size_t n = min(prefs.getBytesLength("ign"), sizeof(ignKeys));
    if (n) { prefs.getBytes("ign", ignKeys, n); ignN = n / 6; }
  }
  if (prefs.isKey("ignss")) {
    size_t n = min(prefs.getBytesLength("ignss"), sizeof(ignSsid));
    if (n) { prefs.getBytes("ignss", ignSsid, n); ignSsidN = n / 33; }
  }
}

void enq(const uint8_t key[6], Cat c, const char* label, const char* name, int8_t rssi, char src, bool ssid,
         bool active, char conf, bool bundle) {
  if (!q) return;
  Evt e;
  memcpy(e.key, key, 6);
  e.cat = c;
  e.rssi = rssi;
  e.src = src;
  e.ssid = ssid;
  e.active = active;
  e.conf = conf;
  e.bundle = bundle;
  strncpy(e.label, label ? label : "", sizeof(e.label) - 1);
  e.label[sizeof(e.label) - 1] = 0;
  strncpy(e.name, name ? name : "", sizeof(e.name) - 1);
  e.name[sizeof(e.name) - 1] = 0;
  xQueueSend(q, &e, 0);  // never block a radio task; drop if full
}

static bool isIgnoredKey(const uint8_t key[6]) {
  for (int i = 0; i < ignN; i++)
    if (!memcmp(ignKeys[i], key, 6)) return true;
  return false;
}
static bool isIgnoredSsid(const char* s) {
  if (!s || !s[0]) return false;
  for (int i = 0; i < ignSsidN; i++)
    if (!strcmp(ignSsid[i], s)) return true;
  return false;
}

// Devices that dropped out, so a return keeps "since" and counts as another visit. Small
// ring: the oldest is forgotten first. Hopping AirTags are not kept (a new address each time).
struct Gone { uint8_t key[6]; uint16_t visits; uint32_t since; };
static const int MAXGONE = 48;
static Gone gone[MAXGONE];
static int goneN = 0, goneHead = 0;
static void remember(const Det& d) {
  if (!d.used || (d.bundle && !d.escalated)) return;
  Gone& g = gone[goneHead];
  memcpy(g.key, d.key, 6);
  g.visits = d.visits;
  g.since = d.since;
  goneHead = (goneHead + 1) % MAXGONE;
  if (goneN < MAXGONE) goneN++;
}
static bool recall(const uint8_t key[6], Gone& out) {
  for (int i = 0; i < goneN; i++) {
    if (!gone[i].visits || memcmp(gone[i].key, key, 6)) continue;
    out = gone[i];
    gone[i].visits = 0;   // back in the table now
    return true;
  }
  return false;
}

static Det* slotFor(const uint8_t key[6]) {
  for (auto& d : dets)
    if (d.used && !memcmp(d.key, key, 6)) return &d;
  Det* old = nullptr;
  for (auto& d : dets) {
    if (!d.used) { d = Det(); return &d; }
    if (d.active && !d.acked) continue;   // kept until acked, even when the table is full
    if (!old || d.lastSeen < old->lastSeen) old = &d;
  }
  if (!old) old = &dets[0];   // all 128 are unacked attacks: something has to give
  remember(*old);
  *old = Det();
  return old;
}

static bool ingest(const Evt& e) {
  Det* d = slotFor(e.key);
  uint32_t now = millis();
  bool fresh = !d->used;
  // An unacked attack is kept until acked (see housekeep). If it comes back after a quiet
  // spell it is a new visit: alert and log again, as a fresh one would.
  bool back = !fresh && d->active && !d->acked && now - d->lastSeen > EVICT_MS;
  if (back) { d->visits++; d->firstSeen = now; }
  if (fresh) {
    d->used = true;
    memcpy(d->key, e.key, 6);
    d->cat = e.cat;
    d->ssidBased = e.ssid;
    d->active = e.active;
    d->conf = e.conf;
    d->src = e.src;
    d->bundle = e.bundle;
    strncpy(d->label, e.label, sizeof(d->label) - 1);
    strncpy(d->name, e.name, sizeof(d->name) - 1);
    d->firstSeen = now;
    d->hits = 0;
    Gone g;
    if (recall(e.key, g)) { d->since = g.since; d->visits = g.visits + 1; }
    else { d->since = now; d->visits = 1; }
    d->place = placeNow;
    d->ignored = isIgnoredKey(e.key) || (e.ssid && isIgnoredSsid(e.name));
  }
  d->lastSeen = now;
  d->rssi = e.rssi;
  d->hits++;
  if (e.name[0]) { memset(d->name, 0, sizeof(d->name)); strncpy(d->name, e.name, sizeof(d->name) - 1); }
  if (d->ignored) return false;
  bool changed = false;
  if (fresh || back) {
    lastDet = now;
    if (d->visits == 1) counts[e.cat]++;   // distinct devices; a return is a visit, not a new one
    if (hooks.fresh) hooks.fresh(*d);
    bool pass = confRank(e.conf) >= filt;
    if (e.active && pass) {
      d->alarm = true;
      d->acked = false;
      sio::out("[det] alert %s at %lus", d->name[0] ? d->name : d->label, (unsigned long)(now / 1000));
      if (hooks.alarm) hooks.alarm(*d);
    } else if (!e.active && pass && e.cat != TRACKER) {
      if (!mute && hooks.passive) hooks.passive(*d);
    }
    changed = true;
  }
  // Following: with you for a while AND still here after you moved. Sitting at home with
  // the neighbours' AirTags is not being followed.
  if (d->cat == TRACKER && !d->escalated && now - d->firstSeen > TRK_FOLLOW_MS &&
      (!placeKnown || d->place != placeNow)) {
    d->escalated = true;
    if (!mute && hooks.follow) hooks.follow(*d);
    changed = true;
  }
  return changed;
}

// Full-screen alerts drop to the tiles on their own: after ALARM_SHOW_MS, or BOOT_GRACE_MS
// for alarms raised while the scanners were starting (a reboot forgets every ack, so the
// user's own Flipper would alert at once). They stay unacked, so the tile stays red until the
// user acks it.
static const uint32_t ALARM_SHOW_MS = 30000, BOOT_GRACE_MS = 10000;
static uint32_t graceUntil = 0xFFFFFFFF;   // until armBootGrace(): everything counts as boot
void armBootGrace() { graceUntil = millis() + BOOT_GRACE_MS; }
static bool quietAlarms(uint32_t now) {
  bool changed = false;
  for (auto& d : dets) {
    if (!d.used || !d.alarm || d.acked) continue;
    bool boot = graceUntil == 0xFFFFFFFF || (int32_t)(d.firstSeen - graceUntil) <= 0;
    if (now - d.firstSeen < (boot ? BOOT_GRACE_MS : ALARM_SHOW_MS)) continue;
    d.alarm = false;
    changed = true;
    sio::out("[det] alert dropped to tiles: %s", d.name[0] ? d.name : d.label);
  }
  return changed;
}

bool drain() {
  if (!q) return false;
  Evt e;
  bool changed = quietAlarms(millis());
  int n = 0;
  while (n < 32 && xQueueReceive(q, &e, 0) == pdTRUE) {
    changed |= ingest(e);
    n++;
  }
  return changed;
}

void housekeep() {
  uint32_t now = millis();
  for (auto& d : dets) {
    if (!d.used) continue;
    if (d.alarm && !d.acked && now - d.lastSeen > ALARM_STANDDOWN_MS) d.alarm = false;
    uint32_t lim = d.cat == TRACKER ? EVICT_TRK_MS : EVICT_MS;
    // Unacked attacks and hacker devices stay until the user acks them: a red tile always
    // has an entry behind it.
    if (now - d.lastSeen > lim && !d.alarm && !(d.active && !d.acked)) { remember(d); d = Det(); }
  }
}

Det& at(int i) { return dets[i]; }

bool alarmActive() {
  for (auto& d : dets)
    if (d.used && d.alarm && !d.acked) return true;
  return false;
}

int activeAttackIdx() {
  int b = -1;
  uint32_t t = 0;
  for (int i = 0; i < MAX; i++) {
    Det& d = dets[i];
    if (d.used && d.alarm && !d.acked && d.lastSeen >= t) { t = d.lastSeen; b = i; }
  }
  return b;
}

bool ackedAttackPresent() {
  uint32_t now = millis();
  for (auto& d : dets)
    if (d.used && d.active && d.acked && now - d.lastSeen < ALARM_STANDDOWN_MS) return true;
  return false;
}

static bool inBundle(const Det& d) { return d.used && !d.ignored && d.bundle && !d.escalated; }

int bundleCount() {
  int n = 0;
  for (auto& d : dets) if (inBundle(d)) n++;
  return n;
}

// Serious first, then newest on top (FUD ordering). Trackers: longest dwell first, with the
// AirTag bundle as one row on top.
int list(int filter, int* idxs) {
  int n = 0;
  if (filter == TRACKER && bundleCount() > 0) idxs[n++] = ROW_BUNDLE;
  int first = n;
  for (int i = 0; i < MAX; i++) {
    Det& d = dets[i];
    if (!d.used || d.ignored) continue;
    if (filter == FILTER_BUNDLE) { if (!inBundle(d)) continue; }
    else if (filter >= 0) { if (d.cat != filter || inBundle(d)) continue; }
    else if (d.cat == TRACKER || d.acked) continue;
    idxs[n++] = i;
  }
  idxs += first;
  n -= first;
  for (int a = 1; a < n; a++) {
    int v = idxs[a], b = a - 1;
    if (filter == TRACKER || filter == FILTER_BUNDLE) {
      while (b >= 0 && dets[idxs[b]].firstSeen > dets[v].firstSeen) { idxs[b + 1] = idxs[b]; b--; }
    } else {
      while (b >= 0 && ((dets[idxs[b]].active < dets[v].active) ||
                        (dets[idxs[b]].active == dets[v].active && dets[idxs[b]].firstSeen < dets[v].firstSeen))) {
        idxs[b + 1] = idxs[b];
        b--;
      }
    }
    idxs[b + 1] = v;
  }
  return n + first;
}

int trackerCount() {
  int n = 0;
  for (auto& d : dets)
    if (d.used && d.cat == TRACKER && !d.ignored && !inBundle(d)) n++;
  return n + (bundleCount() > 0 ? 1 : 0);
}

uint32_t count(Cat c) { return counts[c]; }
int liveCount(Cat c) {
  if (c == TRACKER) return trackerCount();
  int n = 0;
  for (auto& d : dets) if (d.used && d.cat == c && !d.ignored) n++;
  return n;
}
bool hot(Cat c) {
  uint32_t now = millis();
  for (auto& d : dets)
    // Active ones (attacks, hacker devices) stay hot until acked; passive ones for a minute.
    if (d.used && d.cat == c && !d.acked && !d.ignored && (d.active || now - d.lastSeen < ALARM_STANDDOWN_MS))
      return true;
  return false;
}
uint32_t lastDetectionMs() { return lastDet; }

void ack(int i) { dets[i].acked = true; dets[i].alarm = false; }
void ackAll() {
  for (auto& d : dets)
    if (d.used && d.alarm) { d.acked = true; d.alarm = false; }
}

void ignore(int i) {
  Det& d = dets[i];
  d.ignored = true;
  d.acked = true;
  d.alarm = false;
  if (d.ssidBased) {
    if (!isIgnoredSsid(d.name) && ignSsidN < MAXIGNSS) {
      strncpy(ignSsid[ignSsidN], d.name, 32);
      ignSsid[ignSsidN][32] = 0;
      ignSsidN++;
      prefs.putBytes("ignss", ignSsid, ignSsidN * 33);
    }
  } else if (!isIgnoredKey(d.key) && ignN < MAXIGN) {
    memcpy(ignKeys[ignN++], d.key, 6);
    prefs.putBytes("ign", ignKeys, ignN * 6);
  }
}

void snoozeAll() {
  for (auto& d : dets)
    if (d.used) { d.acked = true; d.alarm = false; d.escalated = true; }
}

void clearAll() {
  for (auto& d : dets) d = Det();
  goneN = goneHead = 0;
  memset(counts, 0, sizeof(counts));
  lastDet = millis();
}

void clearIgnores() {
  ignN = ignSsidN = 0;
  prefs.remove("ign");
  prefs.remove("ignss");
  for (auto& d : dets) d.ignored = false;
}
int ignoreCount() { return ignN + ignSsidN; }

static bool muteAvail = false;
void setMuteAvailable(bool yes) { muteAvail = yes; }
// Mute silences the buzzer. With no buzzer there is nothing to mute, so it never applies.
bool muted() { return mute && muteAvail; }
void setMuted(bool m) { mute = m; prefs.putBool("mute", m); }
uint8_t alertFilter() { return filt; }
void setAlertFilter(uint8_t f) { filt = f % 3; prefs.putUChar("alf", filt); }
const char* alertFilterName(uint8_t f) {
  static const char* n[3] = {"ALL", "MED", "HIGH"};
  return n[f % 3];
}

bool isTrackable(const Det& d) {
  return d.used && !d.ssidBased && (d.cat == TRACKER || d.cat == CAMERA || d.cat == HACKDEV);
}

}  // namespace det
