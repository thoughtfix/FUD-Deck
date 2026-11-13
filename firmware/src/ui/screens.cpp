// All screens. Radio features plug into Toys and the detection hooks in later milestones.
#include <Preferences.h>
#include "../board/leds.h"
#include "../board/post.h"
#include "../board/power.h"
#include "../core/detections.h"
#include "../core/ledstatus.h"
#include "../core/scanner.h"
#include "../core/rfwatch.h"
#include "../core/sdlog.h"
#include "../core/clock.h"
#include "../core/zuse.h"
#include "../board/serial_io.h"
#include "../board/pins.h"
#include "face.h"
#include "toys.h"
#include "ui.h"
#include "widgets.h"

using namespace theme;
using widgets::ListView;

#define FUD_VERSION "0.5b"

static Preferences prefs;

// ---------------------------------------------------------------- helpers

// Zuse's face, drawn with shapes (face.cpp). 162 px leaves room for the tiles below.
static const int FACE_S = 162, FACE_X = (W - FACE_S) / 2, FACE_Y = HEADER_H + 4;
static void drawFace(const char* name, uint16_t bg) { face::draw(name, FACE_X, FACE_Y, FACE_S, bg); }

// Size-1 text wrapped at spaces. '\n' forces a break. Returns the y below the last line.
using widgets::drawWrapped;

static void radioSummary(char* b, size_t len) {
  snprintf(b, len, "nRF24 x%d  CC1101 %s  SD %s", post::nrfCount(), post::cc1101() ? "ok" : "--",
           post::sd() ? "ok" : "--");
}

static const char* srcName(char s) {
  switch (s) {
    case 'B': return "Bluetooth LE";
    case 'W': return "Wi-Fi";
    case 'S': return "433 MHz (CC1101)";
    case 'N': return "2.4 GHz (nRF24)";
    default: return "?";
  }
}

// Education first: what each kind of detection means and what to do about it.
static const char* catAbout(uint8_t c) {
  switch (c) {
    case det::TRACKER:
      return "A Bluetooth item tracker (AirTag, Tile, SmartTag and friends). Most belong to people "
             "nearby. One that stays with you for a long time, across places, is the one worth checking.";
    case det::CAMERA:
      return "Something that identifies as a camera or surveillance product: license plate readers, "
             "body cams, camera glasses. Usually lawful. Good to know it is there.";
    case det::DRONE:
      return "A drone broadcasting Remote ID, the digital license plate most drones must send. It "
             "shows a drone is near, not what it is doing.";
    case det::SUBGHZ:
      return "Activity on 433 MHz: car key fobs, tire pressure sensors, doorbells, weather stations. "
             "Repeated identical bursts can mean someone is replaying a signal.";
    case det::HACKDEV:
      return "A device that identifies as a hacking tool (Flipper Zero, WiFi Pineapple and similar). "
             "Owning one is legal in most places (check your local laws). Seeing one is your cue to watch your own gear.";
    case det::ATTACK:
      return "An attack in progress, like a Wi-Fi deauthentication flood that kicks devices off "
             "their network, or a jammer drowning the band. Move away or switch to wired.";
    default: return "";
  }
}

// ---------------------------------------------------------------- detection hooks

static const char* faceFor(uint8_t cat) {
  switch (cat) {
    case det::CAMERA: return "camera";
    case det::DRONE: return "shockedface";
    case det::SUBGHZ: return "tongueface";
    default: return "redface";
  }
}
static void onPassive(const det::Det& d) {
  zuse::react(faceFor(d.cat));
  ledstatus::flashCat(d.src, d.cat);
  ui::dirty();
}
static void onAlarm(const det::Det& d) {
  ledstatus::flashCat(d.src, d.cat);   // the all-red pulse takes over from here
  if (ui::current() != ScreenId::Home) ui::notify("ALERT  hold LEFT: home");
  ui::dirty();
}
static void onFollow(const det::Det& d) {
  zuse::react("neutralface");
  ledstatus::flashCat(d.src, d.cat, 3000);
  // Several often trip together right after a move: one banner with a count.
  static uint32_t last = 0;
  static int n = 0;
  static char msg[2][32];   // two buffers: Home redraws the banner when the pointer changes
  n = millis() - last < 10000 ? n + 1 : 1;
  last = millis();
  char* m = msg[n & 1];
  if (n == 1) snprintf(m, 32, "a tracker is following you");
  else snprintf(m, 32, "%d trackers following you", n);
  ui::notify(m);
}

// ---------------------------------------------------------------- Home

static const char* homeFace = nullptr;
static bool homeAlarm = false;
static const char* homeToast = nullptr;
static const char* homeSay = nullptr;
static char homeSayText[80];   // what the bubble shows: zuse reuses one buffer for every line
static uint32_t homeCountsSig = 0;
static uint32_t homeBatt = 0;

static uint32_t countsSig() {
  uint32_t s = det::trackerCount();
  for (int c = 1; c < det::CAT_COUNT; c++) s = s * 31 + det::liveCount((det::Cat)c);
  return s ^ (det::hot(det::ATTACK) ? 0x1000 : 0) ^ (det::hot(det::HACKDEV) ? 0x2000 : 0);   // red/amber change
}

// Big category tiles: 3 x 2 under Zuse. Each counts what its group lists right now, so tapping
// one always shows something. Red: an attack or hacker device not acked yet. During an alarm
// the area turns into the alert banner.
static const int TILE_Y = FACE_Y + FACE_S + 3, TILE_H = 44, TILE_W = 80;
static const int BAR_Y = TILE_Y + 2 * TILE_H + 2, BAR_H = H - BAR_Y - 2;
static const det::Cat TILE_CAT[6] = {det::CAMERA, det::DRONE, det::HACKDEV, det::ATTACK, det::TRACKER, det::SUBGHZ};
static const char* const BAR_LABEL[3] = {"MENU", "TOYS", "LOG"};

// Bottom bar: real touch buttons, MENU / TOYS / LOG. D-pad: UP = log, DOWN = menu.
static void homeBar() {
  tft.fillRect(0, BAR_Y, W, H - BAR_Y, BG);
  for (int i = 0; i < 3; i++) {
    int x = i * 80;
    tft.drawRoundRect(x + 3, BAR_Y + 1, 74, BAR_H, 6, FG);
    tft.setTextSize(2);
    tft.setTextColor(FG, BG);
    tft.setTextDatum(textdatum_t::middle_center);
    tft.drawString(BAR_LABEL[i], x + 40, BAR_Y + 1 + BAR_H / 2);
    tft.setTextDatum(textdatum_t::top_left);
  }
}

static void homeCounts(bool alarm) {
  uint16_t bg = alarm ? ALERT : BG, fg = alarm ? BG : FG;
  tft.fillRect(0, TILE_Y, W, 2 * TILE_H + 2, bg);
  char b[40];
  if (alarm) {
    int i = det::activeAttackIdx();
    const char* lbl = i >= 0 ? (det::at(i).name[0] ? det::at(i).name : det::at(i).label) : "THREAT";
    tft.setTextColor(fg, bg);
    tft.setTextSize(4);
    tft.drawString("ALERT", 8, TILE_Y + 6);
    tft.setTextSize(2);
    snprintf(b, sizeof(b), "%.19s", lbl);
    tft.drawString(b, 8, TILE_Y + 46);
    tft.setTextSize(1);
    tft.drawString("tap or CENTER to see it", 8, TILE_Y + 72);
    return;
  }
  struct T { det::Cat c; const char* tag; };
  static const T tiles[6] = {{det::CAMERA, "CAMS"}, {det::DRONE, "DRONE"}, {det::HACKDEV, "HACK"},
                             {det::ATTACK, "ATTK"}, {det::TRACKER, "TRAK"}, {det::SUBGHZ, "SUBG"}};
  for (int i = 0; i < 6; i++) {
    int x = (i % 3) * TILE_W, y = TILE_Y + (i / 3) * TILE_H;
    // What the group lists now, so a tile never points at an empty list. Since-boot totals
    // are on the Groups screen.
    uint32_t n = det::liveCount(tiles[i].c);
    bool hot = (tiles[i].c == det::ATTACK || tiles[i].c == det::HACKDEV) && det::hot(tiles[i].c);
    uint16_t c = hot ? ALERT : (n ? FG : DIM);
    tft.drawRoundRect(x + 3, y + 2, TILE_W - 6, TILE_H - 4, 6, c);
    tft.setTextColor(c, BG);
    tft.setTextSize(2);
    tft.setTextDatum(textdatum_t::top_center);
    tft.drawString(tiles[i].tag, x + TILE_W / 2, y + 4);
    tft.setTextSize(3);
    snprintf(b, sizeof(b), "%lu", (unsigned long)n);
    tft.drawString(b, x + TILE_W / 2, y + 18);
    tft.setTextDatum(textdatum_t::top_left);
  }
}

// Header badges: which radios are listening right now, lit green when on.
//   B = Bluetooth LE   W = Wi-Fi   N = nRF24 2.4 GHz   S = sub-GHz (CC1101)
static uint8_t radioMask() {
  return (scanner::bleActive() ? 1 : 0) | (scanner::wifiActive() ? 2 : 0) |
         (rfwatch::nrfRadios() ? 4 : 0) | (rfwatch::ccActive() ? 8 : 0);
}
static uint8_t homeRadio = 0xFF;
static void homeMuteBadge() {
  tft.fillRect(140, 4, 36, HEADER_H - 8, BG);
  tft.setTextSize(1);
  uint8_t m = radioMask();
  homeRadio = m;
  const char* L = "BWNS";
  for (int i = 0; i < 4; i++) {
    tft.setTextColor((m >> i) & 1 ? GOOD : DIM, BG);
    char b[2] = {L[i], 0};
    tft.drawString(b, 141 + i * 7, 10);
  }
  if (!det::muted()) return;
  tft.setTextColor(WARN, BG);
  tft.drawString("M", 170, 10);   // muted
}

// Zuse's speech bubble: white text on black with a white outline, sized to the text, bottom edge fixed over the
// lower part of his face. Wraps at 36 characters, up to 3 lines.
static const int BUB_BOTTOM = FACE_Y + FACE_S - 4, BUB_PAD = 6, BUB_LH = 12, BUB_COLS = 36;
static const uint16_t BUB_BG = 0x0000, BUB_FG = 0xFFFF;   // white on black
static int bubX, bubY, bubW, bubH;   // last drawn box, cleared when the bubble changes or goes
static void homeBubbleClear(uint16_t bg) {
  if (bubW) tft.fillRect(bubX, bubY - 6, bubW, bubH + 6, bg);   // + the tail above it
  bubW = 0;
}
static void homeBubbleDraw(const char* t) {
  tft.setTextSize(1);
  char lines[3][BUB_COLS + 1];
  int n = 0, wmax = 0;
  for (const char* s = t; n < 3 && *s; n++) {
    int take = 0, sp = -1;
    while (s[take] && take < BUB_COLS) { if (s[take] == ' ') sp = take; take++; }
    if (s[take] && sp > 0) take = sp;
    snprintf(lines[n], sizeof(lines[n]), "%.*s", take, s);
    wmax = max(wmax, (int)tft.textWidth(lines[n]));
    s += take;
    while (*s == ' ') s++;
  }
  bubW = wmax + 2 * BUB_PAD;
  bubH = n * BUB_LH + 2 * BUB_PAD - 4;
  bubX = (W - bubW) / 2;
  bubY = BUB_BOTTOM - bubH;
  tft.fillRoundRect(bubX, bubY, bubW, bubH, 5, BUB_BG);
  tft.drawRoundRect(bubX, bubY, bubW, bubH, 5, BUB_FG);   // outline, so it reads over black too
  tft.fillTriangle(W / 2 - 6, bubY, W / 2 + 6, bubY, W / 2, bubY - 6, BUB_BG);   // tail, outlined
  tft.drawLine(W / 2 - 6, bubY, W / 2, bubY - 6, BUB_FG);
  tft.drawLine(W / 2, bubY - 6, W / 2 + 6, bubY, BUB_FG);
  tft.drawFastHLine(W / 2 - 5, bubY, 11, BUB_BG);
  tft.setTextColor(BUB_FG, BUB_BG);
  for (int i = 0; i < n; i++) tft.drawString(lines[i], bubX + BUB_PAD, bubY + BUB_PAD + i * BUB_LH);
}

// What Zuse might say. Moderate: at most every 3 minutes, never during an alert.
static uint32_t chatLast = 0;
static bool chatGreeted = false, chatClockHint = false;
static uint32_t chatCams = 0;
static void homeChatter() {
  uint32_t now = millis();
  if (det::alarmActive() || zuse::sayText() || zuse::toastText()) return;
  char b[80];
  if (!chatGreeted && now > 6000) {
    chatGreeted = true;
    int radios = (int)scanner::bleActive() + (int)scanner::wifiActive() + (rfwatch::nrfRadios() ? 1 : 0) +
                 (rfwatch::ccActive() ? 1 : 0);
    if (clk::trusted()) {
      int y, mo, d, h, mi;
      clk::localFields(y, mo, d, h, mi);
      snprintf(b, sizeof(b), "Good %s. Listening on %d radios.", h < 12 ? "morning" : h < 18 ? "afternoon" : "evening", radios);
    } else snprintf(b, sizeof(b), "Hello. Listening on %d radios. Nothing leaves this board.", radios);
    zuse::say(b, 6000);
    chatLast = now;
    return;
  }
  if (now - chatLast < 180000) return;
  chatLast = now;
  int pct = power::percent();
  uint32_t cams = det::count(det::CAMERA);
  if (pct >= 0 && pct < 20 && !power::charging()) snprintf(b, sizeof(b), "Battery at %d%%. A charger would be nice.", pct);
  else if (cams > chatCams) { snprintf(b, sizeof(b), "A camera showed up. Groups > CAMERAS has the details."); chatCams = cams; }
  else if (!clk::isSet() && !chatClockHint) { snprintf(b, sizeof(b), "Set my clock in Settings and my logs get real times."); chatClockHint = true; }
  else if (det::bundleCount() >= 10) snprintf(b, sizeof(b), "%d Find My beacons around. Normal near phones and AirPods.", det::bundleCount());
  else if (rfwatch::busyPct() >= 30) snprintf(b, sizeof(b), "2.4 GHz is busy: %d%%. Lots of Wi-Fi nearby.", rfwatch::busyPct());
  else if (now - det::lastDetectionMs() > 5UL * 60 * 1000) snprintf(b, sizeof(b), "Quiet in here. I like it.");
  else return;
  zuse::say(b);
}

// Toast: a red strip under the header, over Zuse's forehead (not his eyes). One big line if
// it fits, else two big lines split at the space nearest the middle, else one small line.
static const int TOAST_Y = HEADER_H + 2;
static int toastH = 0;   // height drawn, so it can be cleared
static void homeToastDraw(const char* t) {
  const int room = W - 16;
  char a[40], b[40];
  int lines = 1, size = 2;
  tft.setTextSize(2);
  if (tft.textWidth(t) > room) {
    int n = strlen(t), cut = -1;
    for (int i = 0; i < n; i++)
      if (t[i] == ' ' && (cut < 0 || abs(i - n / 2) < abs(cut - n / 2))) cut = i;
    snprintf(a, sizeof(a), "%.*s", cut < 0 ? 0 : cut, t);
    snprintf(b, sizeof(b), "%s", cut < 0 ? "" : t + cut + 1);
    if (cut >= 0 && tft.textWidth(a) <= room && tft.textWidth(b) <= room) lines = 2;
    else size = 1;
  }
  toastH = lines == 2 ? 38 : 22;
  tft.fillRoundRect(2, TOAST_Y, W - 4, toastH, 6, ALERT);
  tft.drawRoundRect(2, TOAST_Y, W - 4, toastH, 6, 0xFFFF);
  tft.setTextSize(size);
  tft.setTextColor(0xFFFF, ALERT);
  tft.setTextDatum(textdatum_t::middle_center);
  if (lines == 2) {
    tft.drawString(a, W / 2, TOAST_Y + 11);
    tft.drawString(b, W / 2, TOAST_Y + 27);
  } else tft.drawString(t, W / 2, TOAST_Y + toastH / 2);
  tft.setTextDatum(textdatum_t::top_left);
}

static void homeDraw(bool full) {
  bool alarm = det::alarmActive();
  const char* f = zuse::face();
  const char* t = zuse::toastText();
  if (full || alarm != homeAlarm) {
    tft.fillScreen(alarm ? ALERT : BG);
    widgets::header("FUD-DECK", false, true);
    homeFace = nullptr;
    homeCountsSig = ~countsSig();
    homeBar();
    homeAlarm = alarm;
  }
  const char* sp = zuse::sayText();
  bool sayChanged = (sp != homeSay) || (sp && strcmp(sp, homeSayText));
  bool faceRedrawn = false;
  if (f != homeFace || (homeToast && !t) || (homeSay && sayChanged)) {
    faceRedrawn = true;
    // The bubble can be wider than the face: clear its box when it changes or goes away.
    homeBubbleClear(alarm ? ALERT : BG);
    if (homeToast && !t) tft.fillRect(0, TOAST_Y, W, toastH, alarm ? ALERT : BG);   // same for the toast
    drawFace(f, alarm ? ALERT : BG);
    homeFace = f;
  }
  if (sp && !alarm && (sayChanged || faceRedrawn)) homeBubbleDraw(sp);   // stays on top of the face
  homeSay = sp;
  snprintf(homeSayText, sizeof(homeSayText), "%s", sp ? sp : "");
  if (t && (t != homeToast || faceRedrawn)) homeToastDraw(t);   // a new face would cover it
  homeToast = t;
  uint32_t sig = countsSig() ^ (det::muted() ? 0x55 : 0) ^ (alarm ? 0xAA : 0);
  if (sig != homeCountsSig) { homeCounts(alarm); homeMuteBadge(); homeCountsSig = sig; }
  if (millis() - homeBatt > 5000) { homeBatt = millis(); widgets::headerBattery(); }
}

static void toggleMute() {
  det::setMuted(!det::muted());
  zuse::react(det::muted() ? "neutralface" : "grinningface");
  ui::notify(det::muted() ? "MUTED" : "ALERTS ON");
}

// CENTER or a tap: straight to Groups (an active alert comes first). UP log, DOWN/RIGHT menu.
static void homeKey(Key k) {
  if (k == Key::Center) ui::go(det::alarmActive() ? ScreenId::Attack : ScreenId::Groups);
  else if (k == Key::Up) ui::go(ScreenId::Log);
  else if (k == Key::Down || k == Key::Right) ui::go(ScreenId::Menu);
  else if (k == Key::HoldCenter && post::buzzer()) toggleMute();   // nothing to mute without a buzzer
}
static void openGroup(det::Cat c);   // Log, filtered (defined with the Log screen)

// Touch map: [=] settings, Zuse = groups, a tile = that group, bottom bar = LOG/TOYS/MENU.
// During an alert, anything above the bar opens the alert.
static void homeTap(int x, int y) {
  if (widgets::hitHamburger(x, y)) { ui::go(ScreenId::Settings); return; }
  if (y <= HEADER_H) return;
  if (y >= BAR_Y) {
    int b = min(2, x / 80);
    if (b == 0) ui::go(ScreenId::Menu);
    else if (b == 1) ui::go(ScreenId::Toys);
    else openGroup((det::Cat)-1);
    return;
  }
  if (det::alarmActive()) { ui::go(ScreenId::Attack); return; }
  if (y < TILE_Y) { ui::go(ScreenId::Groups); return; }
  int col = min(2, x / TILE_W), row = (y - TILE_Y) / TILE_H;
  openGroup(TILE_CAT[min(1, row) * 3 + col]);
}
// Idle animations, now and then on a quiet Home: a wheel turn (features spin inside the rim)
// or a radar sweep. Anything happening (bubble, toast, alert, new face, sleep) ends it.
enum : uint8_t { ANIM_NONE, ANIM_SPIN, ANIM_SWEEP };
static uint8_t animKind = ANIM_NONE;
static uint32_t animStart = 0, animNext = 0, animFrame = 0;
static uint8_t animWant = ANIM_NONE;   // dev: the kind to start next, else random
static const uint32_t SPIN_MS = 1600, SWEEP_MS = 2600;
// 30 to 90 s: the screen dims after 2 min untouched, so this must come sooner to be seen.
static void animSchedule() { animNext = millis() + 30000 + esp_random() % 60000; }
static void animStop() {
  if (animKind == ANIM_NONE) return;
  animKind = ANIM_NONE;
  homeFace = nullptr;   // the normal face comes back on the next draw
  ui::dirty();
  animSchedule();
}
void homeAnimNow(int kind) {   // dev: start one on the next tick (1 spin, 2 sweep, 0 random)
  animStop();
  animWant = (uint8_t)kind;
  animNext = millis();
}
static void homeAnimTick() {
  uint32_t now = millis();
  bool calm = !det::alarmActive() && !zuse::sayText() && !zuse::toastText() && !ui::asleep() &&
              zuse::face() == homeFace && homeFace;
  if (animKind != ANIM_NONE) {
    uint32_t t = now - animStart, dur = animKind == ANIM_SPIN ? SPIN_MS : SWEEP_MS;
    if (!calm || t >= dur) { animStop(); return; }
    if (now - animFrame < 33) return;   // ~30 fps at most; a frame takes ~20 ms
    animFrame = now;
    float u = (float)t / dur;
    float angle = animKind == ANIM_SPIN ? 180.0f * (1 - cosf(u * (float)M_PI)) : 0;   // eased, one turn
    float sweep = animKind == ANIM_SWEEP ? fmodf(270.0f + 360.0f * u, 360.0f) : -1;   // from 12 o'clock
    if (!face::drawAnim(homeFace, FACE_X, FACE_Y, FACE_S, BG, angle, sweep)) animStop();
    return;
  }
  if (!calm || zuse::idleBusy() || (int32_t)(now - animNext) < 0) return;
  // Start one. Zuse's blinks and glances wait until it is done, or they would end it.
  animKind = animWant ? animWant : (esp_random() & 1) ? ANIM_SPIN : ANIM_SWEEP;
  animWant = ANIM_NONE;
  animStart = animFrame = now;
  zuse::holdIdle((animKind == ANIM_SPIN ? SPIN_MS : SWEEP_MS) + 500);
  if (!face::drawAnim(homeFace, FACE_X, FACE_Y, FACE_S, BG, 0, -1)) { animKind = ANIM_NONE; animSchedule(); }
}

static void homeTick() {
  zuse::tick(true);
  homeChatter();
  homeAnimTick();
  const char* sp = zuse::sayText();
  if ((sp != nullptr) != (homeSay != nullptr) || (sp && strcmp(sp, homeSayText))) ui::dirty();
  if (zuse::face() != homeFace || zuse::toastText() != homeToast || det::alarmActive() != homeAlarm) ui::dirty();
  if (radioMask() != homeRadio) homeMuteBadge();
  static uint32_t last = 0;
  if (millis() - last > 1000) { last = millis(); ui::dirty(); }   // counts, battery
}

// ---------------------------------------------------------------- Log (and filtered views)

static int logFilter = -1;                 // -1 = active log, else a category
static int logIdx[det::MAX];
static int logN = 0;
static int curDet = -1;
static ListView logLvMain, logLvBundle;
static uint32_t logSig = 0;
// The Bundle screen reuses the Log code with its own filter and selection.
static bool viewBundle = false;
#define LOGF (viewBundle ? det::FILTER_BUNDLE : logFilter)
#define logLv (viewBundle ? logLvBundle : logLvMain)

static uint32_t logSignature() {
  uint32_t s = logN;
  for (int i = 0; i < logN; i++) {
    if (logIdx[i] == det::ROW_BUNDLE) { s = s * 31 + det::bundleCount(); continue; }
    s = s * 31 + logIdx[i] * 7 + det::at(logIdx[i]).hits + det::at(logIdx[i]).alarm;
  }
  return s;
}

static void logLabel(int i, char* b, size_t n, uint16_t* color) {
  if (logIdx[i] == det::ROW_BUNDLE) {
    snprintf(b, n, "AirTags x%d   >", det::bundleCount());
    *color = DIM;
    return;
  }
  det::Det& d = det::at(logIdx[i]);
  const char* nm = d.name[0] ? d.name : d.label;
  // Red: needs a look (alarm, or an attack / hacker device not acked yet). Dim: acked.
  if (d.alarm || (d.active && !d.acked)) *color = ALERT;
  else if (d.acked) *color = DIM;
  // Trackers: signal (closer = higher) and how long it has been around.
  if (d.cat == det::TRACKER) snprintf(b, n, "%.7s %4d %3lum", nm, d.rssi, (unsigned long)((millis() - d.firstSeen) / 60000));
  else if (d.hits > 1) snprintf(b, n, "%s %.9s x%u", det::CAT_TAG[d.cat], nm, (unsigned)d.hits);
  else snprintf(b, n, "%s %.14s", det::CAT_TAG[d.cat], nm);
}

static void logEnter() {
  viewBundle = false;
  logLv.label = logLabel;
  logN = det::list(LOGF, logIdx);
  logSig = logSignature();
}
static void bundleEnter() {
  viewBundle = true;
  logLv.label = logLabel;
  logN = det::list(LOGF, logIdx);
  logSig = logSignature();
}
static void logDraw(bool full) {
  if (full) {
    tft.fillScreen(BG);
    widgets::header(LOGF < 0 ? "LOG" : LOGF == det::FILTER_BUNDLE ? "AIRTAGS" : det::CAT_NAME[LOGF], true);
    widgets::footer("CENTER: details   hold LEFT: back");
  }
  if (logN == 0) {
    widgets::clearBody();
    drawWrapped(LOGF < 0 ? "Nothing active right now.\n\nAcknowledged items leave this list. Trackers "
                                "live under Groups > TRACKERS."
                              : "Nothing in this group.",
                8, HEADER_H + 12, W - 16, DIM);
    const int zs = 120;   // Zuse, relaxed: nothing to worry about here
    face::draw("defaultsmile", (W - zs) / 2, HEADER_H + 90, zs, BG);
    return;
  }
  widgets::listDraw(logLv, logN, full);
}
static void openGroup(det::Cat c) {
  logFilter = (int)(int8_t)c;   // -1 = the active log
  logLvMain.sel = logLvMain.top = 0;
  ui::go(ScreenId::Log);
}

static void logOpen(int i) {
  if (logIdx[i] == det::ROW_BUNDLE) { ui::go(ScreenId::Bundle); return; }
  curDet = logIdx[i];
  ui::go(ScreenId::Item);
}
static void logKey(Key k) {
  if (k == Key::Up) { widgets::listMove(logLv, logN, -1); ui::dirty(); }
  else if (k == Key::Down) { widgets::listMove(logLv, logN, 1); ui::dirty(); }
  else if ((k == Key::Center || k == Key::Right) && logN) logOpen(logLv.sel);
}
static void logTap(int x, int y) {
  int i = widgets::listHit(logLv, logN, x, y);
  if (i >= 0) { logLv.sel = i; widgets::listDraw(logLv, logN, false); logOpen(i); }
}
static void logTick() {
  static uint32_t last = 0;
  if (millis() - last < 1000) return;
  last = millis();
  logN = det::list(LOGF, logIdx);
  uint32_t s = logSignature();
  if (s != logSig) { logSig = s; ui::redraw(); }
}

// ---------------------------------------------------------------- Item

static int itemAct = 0;
static const int ACT_Y = 262, ACT_H = 30;

static int itemActions(const char** names) {
  det::Det& d = det::at(curDet);
  int n = 0;
  names[n++] = "Ack";
  names[n++] = "Ignore";
  if (det::isTrackable(d)) names[n++] = "Find";
  names[n++] = "Back";
  return n;
}

// "45s", "12m", "3h05m"
static void agoText(char* b, size_t n, uint32_t ms) {
  uint32_t t = ms / 1000;
  if (t < 60) snprintf(b, n, "%lus", (unsigned long)t);
  else if (t < 3600) snprintf(b, n, "%lum", (unsigned long)(t / 60));
  else snprintf(b, n, "%luh%02lum", (unsigned long)(t / 3600), (unsigned long)(t / 60 % 60));
}

static void itemDrawActions() {
  const char* names[4];
  int n = itemActions(names);
  if (itemAct >= n) itemAct = 0;
  int w = (W - 8) / n;
  tft.fillRect(0, ACT_Y - 2, W, ACT_H + 4, BG);
  for (int i = 0; i < n; i++) {
    int x = 4 + i * w;
    bool sel = i == itemAct;
    tft.fillRoundRect(x + 2, ACT_Y, w - 4, ACT_H, 5, sel ? FG : BG);
    tft.drawRoundRect(x + 2, ACT_Y, w - 4, ACT_H, 5, FG);
    tft.setTextSize(2);   // smaller when a label would not fit its button ("Ignore" in four)
    if (tft.textWidth(names[i]) > w - 10) tft.setTextSize(1.5f);
    if (tft.textWidth(names[i]) > w - 10) tft.setTextSize(1);
    tft.setTextColor(sel ? BG : FG, sel ? FG : BG);
    tft.setTextDatum(textdatum_t::middle_center);
    tft.drawString(names[i], x + w / 2, ACT_Y + ACT_H / 2);
    tft.setTextDatum(textdatum_t::top_left);
  }
}

static void itemEnter() { itemAct = 0; }
static void itemDraw(bool full) {
  if (curDet < 0 || !det::at(curDet).used) { ui::back(); return; }
  det::Det& d = det::at(curDet);
  if (full) {
    tft.fillScreen(BG);
    widgets::header(det::CAT_TAG[d.cat], true);
    tft.setTextSize(2);
    tft.setTextColor(d.alarm ? ALERT : FG, BG);
    char b[64];
    snprintf(b, sizeof(b), "%.19s", d.name[0] ? d.name : d.label);
    tft.drawString(b, 8, HEADER_H + 8);
    int y = HEADER_H + 30;
    tft.setTextSize(1);
    tft.setTextColor(FG, BG);
    snprintf(b, sizeof(b), "type  %s (%s)  conf %c", d.label, det::CAT_TAG[d.cat], d.conf);
    tft.drawString(b, 8, y); y += 12;
    snprintf(b, sizeof(b), "id    %02X:%02X:%02X:%02X:%02X:%02X", d.key[0], d.key[1], d.key[2], d.key[3], d.key[4], d.key[5]);
    tft.drawString(b, 8, y); y += 12;
    snprintf(b, sizeof(b), "via   %s", srcName(d.src));
    tft.drawString(b, 8, y); y += 12;
    snprintf(b, sizeof(b), "rssi  %d dBm   hits %u", d.rssi, d.hits);
    tft.drawString(b, 8, y); y += 12;
    char since[12], last[12];
    agoText(since, sizeof(since), millis() - d.since);
    agoText(last, sizeof(last), millis() - d.lastSeen);
    snprintf(b, sizeof(b), "seen  %ux since %s ago", (unsigned)d.visits, since);
    tft.drawString(b, 8, y); y += 12;
    snprintf(b, sizeof(b), "last  %s ago", last);
    tft.drawString(b, 8, y); y += 18;
    tft.drawFastHLine(8, y - 4, W - 16, DIM);
    tft.setTextColor(INFO, BG);
    tft.drawString("What is this?", 8, y); y += 13;
    drawWrapped(catAbout(d.cat), 8, y, W - 16, FG);
    widgets::footer("LEFT/RIGHT choose  CENTER do it");
  }
  itemDrawActions();
}
static void itemDo() {
  const char* names[4];
  int n = itemActions(names);
  const char* a = names[itemAct < n ? itemAct : 0];
  if (!strcmp(a, "Ack")) { det::ack(curDet); ui::back(); }
  else if (!strcmp(a, "Ignore")) {
    bool ssid = det::at(curDet).ssidBased;
    det::ignore(curDet);
    ui::back();
    ui::notify(ssid ? "SSID ignored for good" : "ignored for good");
  } else if (!strcmp(a, "Find")) toys::finderLock(curDet);
  else ui::back();
}
static void itemKey(Key k) {
  const char* names[4];
  int n = itemActions(names);
  if (k == Key::Left) { itemAct = (itemAct + n - 1) % n; itemDrawActions(); }
  else if (k == Key::Right) { itemAct = (itemAct + 1) % n; itemDrawActions(); }
  else if (k == Key::Center) itemDo();
}
static void itemTap(int x, int y) {
  if (y < ACT_Y || y > ACT_Y + ACT_H) return;
  const char* names[4];
  int n = itemActions(names);
  itemAct = min(n - 1, (x - 4) / ((W - 8) / n));
  itemDrawActions();
  itemDo();
}

// ---------------------------------------------------------------- Attack

static int attackShow = -1;

static void attackEnter() {
  attackShow = det::activeAttackIdx();
  det::ackAll();   // opening the alert acknowledges it (FUD behavior)
}
static void attackDraw(bool full) {
  if (!full) return;
  tft.fillScreen(BG);
  widgets::header("ALERT", true);
  if (attackShow < 0 || !det::at(attackShow).used) {
    drawWrapped("The alert has cleared.", 8, HEADER_H + 12, W - 16, FG);
    widgets::footer("CENTER: home");
    return;
  }
  det::Det& d = det::at(attackShow);
  tft.setTextSize(3);
  tft.setTextColor(ALERT, BG);
  tft.drawString(d.cat == det::HACKDEV ? "HACKER" : "ATTACK", 8, HEADER_H + 8);
  tft.setTextSize(2);
  char b[48];
  snprintf(b, sizeof(b), "%.19s", d.name[0] ? d.name : d.label);
  tft.drawString(b, 8, HEADER_H + 40);
  tft.setTextSize(1);
  tft.setTextColor(FG, BG);
  int y = HEADER_H + 64;
  snprintf(b, sizeof(b), "via %s   rssi %d dBm", srcName(d.src), d.rssi);
  tft.drawString(b, 8, y); y += 12;
  snprintf(b, sizeof(b), "id %02X:%02X:%02X:%02X:%02X:%02X   x%u", d.key[0], d.key[1], d.key[2], d.key[3], d.key[4], d.key[5], d.hits);
  tft.drawString(b, 8, y); y += 18;
  tft.setTextColor(INFO, BG);
  tft.drawString("What is this?", 8, y); y += 13;
  y = drawWrapped(catAbout(d.cat), 8, y, W - 16, FG);
  drawWrapped("Acknowledged. The red alert clears. If it is still around, Zuse will show it again "
              "now and then.", 8, y + 8, W - 16, DIM);
  widgets::footer("tap or CENTER: home");
}
static void attackKey(Key k) {
  if (k == Key::Center || k == Key::Right) ui::home();
}
static void attackTap(int, int y) { if (y > HEADER_H) ui::home(); }

// ---------------------------------------------------------------- Groups (FUD zoom)

static const det::Cat GROUPS[] = {det::CAMERA, det::DRONE, det::HACKDEV, det::ATTACK, det::TRACKER, det::SUBGHZ};
static const int GROUPS_N = 6;
static ListView grpLv;

static void grpLabel(int i, char* b, size_t n, uint16_t* color) {
  det::Cat c = GROUPS[i];
  static int idx[det::MAX];
  int live = det::list(c, idx);
  if ((c == det::ATTACK || c == det::HACKDEV) && det::hot(c)) *color = ALERT;
  snprintf(b, n, "%-9s %2d/%-3lu", det::CAT_NAME[c], live, (unsigned long)det::count(c));
}
static void grpEnter() { grpLv.label = grpLabel; grpLv.y0 = HEADER_H + 18; grpLv.rows = GROUPS_N; }
static void grpDraw(bool full) {
  if (full) {
    tft.fillScreen(BG);
    widgets::header("GROUPS", true);
    tft.setTextSize(1);
    tft.setTextColor(DIM, BG);
    tft.drawString("now / total since boot", W - 144, HEADER_H + 5);
    widgets::footer("CENTER: open   hold LEFT: back");
    // Zuse in the space under the list, with the face he has on Home.
    const int zs = 64, zy = grpLv.y0 + GROUPS_N * grpLv.rowH + 6;
    face::draw(zuse::face(), (W - zs) / 2, zy, zs, BG);
  }
  widgets::listDraw(grpLv, GROUPS_N, full);
}
static void grpOpen(int i) { logFilter = GROUPS[i]; ui::go(ScreenId::Log); }
static void grpKey(Key k) {
  if (k == Key::Up) { widgets::listMove(grpLv, GROUPS_N, -1); ui::dirty(); }
  else if (k == Key::Down) { widgets::listMove(grpLv, GROUPS_N, 1); ui::dirty(); }
  else if (k == Key::Center || k == Key::Right) grpOpen(grpLv.sel);
}
static void grpTap(int x, int y) {
  int i = widgets::listHit(grpLv, GROUPS_N, x, y);
  if (i >= 0) { grpLv.sel = i; widgets::listDraw(grpLv, GROUPS_N, false); grpOpen(i); }
}
static void grpTick() {
  static uint32_t last = 0;
  if (millis() - last > 2000) {
    last = millis();
    grpLv.lastSel = -1;
    widgets::listDraw(grpLv, GROUPS_N, true);   // counts only, no flicker of the header
  }
}

// ---------------------------------------------------------------- Menu

static const char* MENU_ITEMS[] = {"Log", "Groups", "Toys", "Settings", "Snooze all", "Clear log"};
static const int MENU_N = 6;
static ListView menuLv;

static void menuLabel(int i, char* b, size_t n, uint16_t*) { snprintf(b, n, "%s", MENU_ITEMS[i]); }

static void menuEnter() {
  menuLv.y0 = HEADER_H + 46;
  menuLv.rows = 7;
  menuLv.label = menuLabel;
}
static void menuDraw(bool full) {
  if (full) {
    tft.fillScreen(BG);
    widgets::header("MENU", true);
    char b[48];
    tft.setTextSize(1);
    tft.setTextColor(FG, BG);
    int pct = power::percent();
    if (power::volts() > 0) snprintf(b, sizeof(b), "Battery %d%%  %.2fV%s", pct, power::volts(), power::charging() ? "  charging" : "");
    else snprintf(b, sizeof(b), "Battery %d%%%s", pct, power::charging() ? "  charging" : "");
    tft.drawString(b, 8, HEADER_H + 6);
    radioSummary(b, sizeof(b));
    tft.drawString(b, 8, HEADER_H + 18);
    if (rfwatch::nrfRadios() || rfwatch::ccActive()) {
      snprintf(b, sizeof(b), "2.4 GHz %d%% busy  433 floor %d dBm", rfwatch::busyPct(), rfwatch::ccFloor());
      tft.setTextColor(DIM, BG);
      tft.drawString(b, 8, HEADER_H + 30);
    }
    tft.setTextColor(det::muted() ? WARN : DIM, BG);
    if (det::muted()) tft.drawString("alerts MUTED (hold CENTER on Home)", 120, HEADER_H + 6);
    tft.drawFastHLine(0, HEADER_H + 42, W, DIM);
    widgets::footer("CENTER: open    hold LEFT: back");
  }
  widgets::listDraw(menuLv, MENU_N, full);
}
static void menuOpen(int i) {
  switch (i) {
    case 0: logFilter = -1; ui::go(ScreenId::Log); break;
    case 1: ui::go(ScreenId::Groups); break;
    case 2: ui::go(ScreenId::Toys); break;
    case 3: ui::go(ScreenId::Settings); break;
    case 4: det::snoozeAll(); zuse::react("grinningface"); ui::home(); ui::notify("BASELINED"); break;
    case 5: det::clearAll(); ui::home(); ui::notify("LOG CLEARED"); break;
  }
}
static void menuKey(Key k) {
  if (k == Key::Up) { widgets::listMove(menuLv, MENU_N, -1); ui::dirty(); }
  else if (k == Key::Down) { widgets::listMove(menuLv, MENU_N, 1); ui::dirty(); }
  else if (k == Key::Center || k == Key::Right) menuOpen(menuLv.sel);
}
static void menuTap(int x, int y) {
  int i = widgets::listHit(menuLv, MENU_N, x, y);
  if (i >= 0) { menuLv.sel = i; widgets::listDraw(menuLv, MENU_N, false); menuOpen(i); }
}

// ---------------------------------------------------------------- Toys

struct Toy { const char* name; const char* about; };
static const Toy TOYS[] = {
  {"Waterfall", "A live scrolling spectrum. Each row is one sweep, newest on top, brighter means "
                "more energy. 2.4 GHz uses both nRF24 radios at once, each covering half the band. "
                "433 MHz uses the CC1101."},
  {"SSID Scan", "The Wi-Fi networks around you, strongest first, with channel, signal strength and "
                "whether they are open. Passive: FUD-Deck only listens."},
  {"Traffic", "How busy the 2.4 GHz band is over time, green to red, with a running peak. Wi-Fi, "
              "Bluetooth, microwaves and wireless gear all add up here."},
  {"Finder", "Walk toward a signal. Pick a recent tracker, camera or device and follow the hotter / "
             "colder meter. AirTags change address every few minutes, so expect to re-pick them."},
  {"TV-B-Gone", "Sends TV power-off codes for the common brands through the IR LED. The one "
                "transmit in FUD-Deck. A tool, at worst a prank."},
  {"IR Decoder", "Point any remote at the board. Shows the protocol, address, command and raw "
                 "timing, and explains what a 38 kHz carrier and a repeat code are."},
};
static const int TOYS_N = sizeof(TOYS) / sizeof(TOYS[0]);
static ListView toysLv;
static int toyPicked = 0;

// One line about each toy for the space beside Zuse, and its colour: cyan for the radio toys,
// orange for infrared.
static const char* const TOY_BLURB[] = {
  "Live spectrum. 2.4 GHz on both nRF24s, or 433 MHz.",
  "Wi-Fi networks around you, strongest first.",
  "How busy 2.4 GHz is, over time.",
  "Walk toward a tracker, camera or device.",
  "Turn off TVs. The one thing FUD-Deck sends.",
  "Point a remote at it and see how it talks.",
};
static const uint16_t TOY_COLOR[] = {INFO, INFO, INFO, INFO, WARN, WARN};
static void toysLabel(int i, char* b, size_t n, uint16_t* color) {
  snprintf(b, n, "%s", TOYS[i].name);
  *color = TOY_COLOR[i];
}
static void toysEnter() { toysLv.label = toysLabel; toysLv.rows = TOYS_N; }
static void toysBlurb() {
  const int zs = 72, y = toysLv.y0 + TOYS_N * toysLv.rowH + 4;
  tft.fillRect(0, y, W - zs - 8, zs, BG);
  drawWrapped(TOY_BLURB[toysLv.sel], 10, y + 10, W - zs - 22, TOY_COLOR[toysLv.sel]);
}
static void toysDraw(bool full) {
  static int shown = -1;
  if (full) {
    tft.fillScreen(BG);
    widgets::header("TOYS", true);
    widgets::footer("CENTER: open    hold LEFT: back");
    const int zs = 72;
    face::draw("grinningface", W - zs - 6, toysLv.y0 + TOYS_N * toysLv.rowH + 4, zs, BG);
    shown = -1;
  }
  widgets::listDraw(toysLv, TOYS_N, full);
  if (toysLv.sel != shown) { shown = toysLv.sel; toysBlurb(); }
}
static void toysOpen(int i) {
  toyPicked = i;
  const char* n = TOYS[i].name;
  if (!strcmp(n, "Waterfall")) ui::go(ScreenId::Waterfall);
  else if (!strcmp(n, "Traffic")) ui::go(ScreenId::Traffic);
  else if (!strcmp(n, "SSID Scan")) ui::go(ScreenId::SsidScan);
  else if (!strcmp(n, "IR Decoder")) ui::go(ScreenId::IrDecode);
  else if (!strcmp(n, "TV-B-Gone")) ui::go(ScreenId::TvbGone);
  else if (!strcmp(n, "Finder")) ui::go(ScreenId::Finder);
  else ui::go(ScreenId::ToyStub);
}
static void toysKey(Key k) {
  if (k == Key::Up) { widgets::listMove(toysLv, TOYS_N, -1); ui::dirty(); }
  else if (k == Key::Down) { widgets::listMove(toysLv, TOYS_N, 1); ui::dirty(); }
  else if (k == Key::Center || k == Key::Right) toysOpen(toysLv.sel);
}
static void toysTap(int x, int y) {
  int i = widgets::listHit(toysLv, TOYS_N, x, y);
  if (i >= 0) { toysLv.sel = i; widgets::listDraw(toysLv, TOYS_N, false); toysOpen(i); }
}

static void stubDraw(bool full) {
  if (!full) return;
  tft.fillScreen(BG);
  widgets::header(TOYS[toyPicked].name, true);
  int y = drawWrapped(TOYS[toyPicked].about, 8, HEADER_H + 12, W - 16, FG);
  drawWrapped("Coming in a later milestone.", 8, y + 12, W - 16, DIM);
  widgets::footer("hold LEFT or tap BACK");
}

// ---------------------------------------------------------------- Settings

static const uint8_t BRIGHT[4] = {255, 160, 80, 25};
static const char* BRIGHT_NAMES[4] = {"HIGH", "MED", "LOW", "DIM"};
static int brightIdx = 0;          // resets every boot, so a dark screen can never lock you out
static bool saverOn = false;
static const uint32_t SLEEP_MS[5] = {30000, 60000, 120000, 300000, 0};
static const char* SLEEP_NAMES[5] = {"30s", "1 min", "2 min", "5 min", "OFF"};
static int sleepIdx = 2;
static void applySaver() { scanner::setSaver(saverOn); rfwatch::setSaver(saverOn); }

// Rows are built per visit: only the ones that apply to this board show up.
enum SetRow : uint8_t { R_BRIGHT, R_SLEEP, R_LEDS, R_ALERTS, R_MUTE, R_IGNORED, R_BATTERY, R_CLOCK, R_TOUCH, R_BUTTONS, R_SDLOG, R_POST, R_ABOUT };
static uint8_t setRows[13];
static int setN = 0;
static ListView setLv;

static void setLabel(int i, char* b, size_t n, uint16_t*) {
  switch (setRows[i]) {
    case R_BRIGHT: snprintf(b, n, "Bright  %s", BRIGHT_NAMES[brightIdx]); break;
    case R_SLEEP: snprintf(b, n, "Sleep   %s", SLEEP_NAMES[sleepIdx]); break;
    case R_LEDS: snprintf(b, n, "LEDs    %s", leds::levelName(leds::level())); break;
    case R_ALERTS: snprintf(b, n, "Alerts  %s", det::alertFilterName(det::alertFilter())); break;
    case R_MUTE: snprintf(b, n, "Mute    %s", det::muted() ? "ON" : "OFF"); break;
    case R_IGNORED: snprintf(b, n, "Ignored %d  clr", det::ignoreCount()); break;
    case R_BATTERY: snprintf(b, n, "Battery %s", saverOn ? "SAVER" : "FULL"); break;
    case R_CLOCK: {
      char t[8];
      clk::formatTime(t, sizeof(t));
      // A guess is the last saved time, stale by however long the power was off.
      snprintf(b, n, "Clock   %s  >", !clk::isSet() ? "not set" : clk::guessed() ? "set me" : t);
      break;
    }
    case R_SDLOG: snprintf(b, n, "SD log  %s", !sdlog::enabled() ? "OFF" : sdlog::present() ? "ON" : "no card"); break;
    case R_TOUCH: snprintf(b, n, "Touch cal    >"); break;
    case R_BUTTONS: snprintf(b, n, "Button test  >"); break;
    case R_POST: snprintf(b, n, "POST report  >"); break;
    case R_ABOUT: snprintf(b, n, "About        >"); break;
  }
}
static void setEnter() {
  setN = 0;
  setRows[setN++] = R_BRIGHT;
  setRows[setN++] = R_SLEEP;
  setRows[setN++] = R_LEDS;
  setRows[setN++] = R_ALERTS;
  if (post::buzzer()) setRows[setN++] = R_MUTE;   // hidden with no buzzer
  setRows[setN++] = R_IGNORED;
  setRows[setN++] = R_BATTERY;
  setRows[setN++] = R_CLOCK;
  setRows[setN++] = R_TOUCH;
  setRows[setN++] = R_BUTTONS;
  setRows[setN++] = R_SDLOG;
  setRows[setN++] = R_POST;
  setRows[setN++] = R_ABOUT;
  setLv.label = setLabel;
}
static void setDraw(bool full) {
  if (full) {
    tft.fillScreen(BG);
    widgets::header("SETTINGS", true);
    widgets::footer("tap or LEFT/RIGHT to change");
  }
  widgets::listDraw(setLv, setN, full);
}
static bool setIsLink(int i) { return setRows[i] == R_POST || setRows[i] == R_ABOUT || setRows[i] == R_CLOCK || setRows[i] == R_TOUCH ||
                                  setRows[i] == R_BUTTONS; }
static void setChange(int i, int dir) {
  switch (setRows[i]) {
    case R_BRIGHT:
      brightIdx = (brightIdx + (dir > 0 ? 1 : 3)) % 4;
      ui::setBacklight(BRIGHT[brightIdx]);
      break;
    case R_SLEEP:
      sleepIdx = (sleepIdx + (dir > 0 ? 1 : 4)) % 5;
      prefs.putUChar("sleep", sleepIdx);
      ui::setSleepAfter(SLEEP_MS[sleepIdx]);
      break;
    case R_LEDS: {
      uint8_t l = (leds::level() + (dir > 0 ? 1 : 3)) % 4;
      leds::setLevel(l);
      prefs.putUChar("led", l);
      for (char c : {'B', 'W', 'S', 'N'}) ledstatus::flash(c, 255, 160, 0, 800);  // preview in amber
      break;
    }
    case R_ALERTS: det::setAlertFilter(det::alertFilter() + (dir > 0 ? 1 : 2)); break;
    case R_MUTE: det::setMuted(!det::muted()); break;
    case R_IGNORED: if (dir > 0) { det::clearIgnores(); ui::notify("ignore list cleared"); } break;
    case R_BATTERY:
      saverOn = !saverOn;
      prefs.putBool("saver", saverOn);
      applySaver();
      ui::notify(saverOn ? "saver: radios take turns" : "full: all radios at once");
      break;
    case R_SDLOG:
      sdlog::setEnabled(!sdlog::enabled());
      if (sdlog::enabled() && sdlog::present()) ui::notify(sdlog::fileName());
      break;
    default: break;
  }
  setLv.lastSel = -1;
  widgets::listDraw(setLv, setN, true);
}
static void setOpen(int i) {
  if (setRows[i] == R_POST) ui::go(ScreenId::PostReport);
  else if (setRows[i] == R_CLOCK) ui::go(ScreenId::SetClock);
  else if (setRows[i] == R_ABOUT) ui::go(ScreenId::About);
  else if (setRows[i] == R_TOUCH) ui::go(ScreenId::TouchCal);
  else if (setRows[i] == R_BUTTONS) ui::go(ScreenId::ButtonTest);
  else setChange(i, 1);
}
static void setKey(Key k) {
  if (k == Key::Up) { widgets::listMove(setLv, setN, -1); ui::dirty(); }
  else if (k == Key::Down) { widgets::listMove(setLv, setN, 1); ui::dirty(); }
  else if (k == Key::Left) setChange(setLv.sel, -1);
  else if (k == Key::Right) { if (!setIsLink(setLv.sel)) setChange(setLv.sel, 1); else setOpen(setLv.sel); }
  else if (k == Key::Center) setOpen(setLv.sel);
}
static void setTap(int x, int y) {
  int i = widgets::listHit(setLv, setN, x, y);
  if (i >= 0) { setLv.sel = i; widgets::listDraw(setLv, setN, false); setOpen(i); }
}

// ---------------------------------------------------------------- SET CLOCK

// Fields: 0 year, 1 month, 2 day, 3 hour, 4 minute, 5 zone.
static int ckVal[5];
static uint8_t ckZone = 0;
static int ckSel = 2;
struct Box { int16_t x, y, w, h; };
static const Box CK_BOX[5] = {{12, 92, 96, 46}, {118, 92, 52, 46}, {178, 92, 52, 46},
                              {50, 150, 60, 46}, {130, 150, 60, 46}};
static const Box CK_ZL = {6, 40, 40, 38}, CK_ZR = {194, 40, 40, 38};
static const Box CK_MINUS = {10, 210, 105, 40}, CK_PLUS = {125, 210, 105, 40};
static const Box CK_SAVE = {10, 258, 220, 40};

static bool inBox(const Box& b, int x, int y) { return x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h; }

static int daysIn(int y, int m) {
  static const int d[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
  return m == 2 && leap ? 29 : d[(m - 1) % 12];
}

static void ckButton(const Box& b, const char* label, bool on) {
  tft.fillRoundRect(b.x, b.y, b.w, b.h, 6, on ? FG : BG);
  tft.drawRoundRect(b.x, b.y, b.w, b.h, 6, FG);
  tft.setTextColor(on ? BG : FG, on ? FG : BG);
  tft.setTextDatum(textdatum_t::middle_center);
  tft.drawString(label, b.x + b.w / 2, b.y + b.h / 2);
  tft.setTextDatum(textdatum_t::top_left);
}

static void ckEnter() {
  clk::localFields(ckVal[0], ckVal[1], ckVal[2], ckVal[3], ckVal[4]);
  ckZone = clk::zone();
  ckSel = 3;   // the hour is what most people fix
}
static void ckDraw(bool full) {
  if (full) {
    tft.fillScreen(BG);
    widgets::header("SET CLOCK", true);
    tft.setTextSize(3);
    tft.setTextColor(DIM, BG);
    tft.drawString(":", 114, 160);
    tft.setTextSize(2);
    ckButton(CK_MINUS, "-", false);
    ckButton(CK_PLUS, "+", false);
    ckButton(CK_SAVE, "SAVE", false);
    widgets::footer("tap a box, then - or +");
  }
  // zone row
  tft.fillRect(0, CK_ZL.y, W, CK_ZL.h, BG);
  tft.setTextSize(2);
  ckButton(CK_ZL, "<", ckSel == 5);
  ckButton(CK_ZR, ">", ckSel == 5);
  tft.setTextColor(ckSel == 5 ? FG : DIM, BG);
  tft.setTextDatum(textdatum_t::middle_center);
  tft.setTextSize(clk::zoneName(ckZone)[11] ? 1.5f : 2.0f);
  tft.drawString(clk::zoneName(ckZone), W / 2, CK_ZL.y + CK_ZL.h / 2);
  tft.setTextDatum(textdatum_t::top_left);
  // fields
  tft.setTextSize(3);
  for (int i = 0; i < 5; i++) {
    char b[8];
    snprintf(b, sizeof(b), i == 0 ? "%04d" : "%02d", ckVal[i]);
    ckButton(CK_BOX[i], b, ckSel == i);
  }
}
static void ckBump(int dir) {
  if (ckSel == 5) { ckZone = clk::zoneStep(ckZone, dir); ui::dirty(); return; }
  static const int lo[5] = {2025, 1, 1, 0, 0}, hi[5] = {2099, 12, 31, 23, 59};
  int top = ckSel == 2 ? daysIn(ckVal[0], ckVal[1]) : hi[ckSel];
  int v = ckVal[ckSel] + dir;
  if (v > top) v = lo[ckSel];
  if (v < lo[ckSel]) v = top;
  ckVal[ckSel] = v;
  if (ckVal[2] > daysIn(ckVal[0], ckVal[1])) ckVal[2] = daysIn(ckVal[0], ckVal[1]);
  ui::dirty();
}
static void ckSave() {
  clk::setZone(ckZone);
  if (clk::setLocal(ckVal[0], ckVal[1], ckVal[2], ckVal[3], ckVal[4])) {
    ui::back();
    ui::notify("clock set");
  } else ui::notify("that time is not valid");
}
static void ckKey(Key k) {
  if (k == Key::Left) { ckSel = (ckSel + 5) % 6; ui::dirty(); }
  else if (k == Key::Right) { ckSel = (ckSel + 1) % 6; ui::dirty(); }
  else if (k == Key::Up) ckBump(1);
  else if (k == Key::Down) ckBump(-1);
  else if (k == Key::Center) ckSave();
}
static void ckTap(int x, int y) {
  for (int i = 0; i < 5; i++) if (inBox(CK_BOX[i], x, y)) { ckSel = i; ui::dirty(); return; }
  if (inBox(CK_ZL, x, y)) { ckSel = 5; ckBump(-1); return; }
  if (inBox(CK_ZR, x, y)) { ckSel = 5; ckBump(1); return; }
  if (inBox(CK_MINUS, x, y)) { ckBump(-1); return; }
  if (inBox(CK_PLUS, x, y)) { ckBump(1); return; }
  if (inBox(CK_SAVE, x, y)) ckSave();
}

// ---------------------------------------------------------------- POST report

static int postTop = 0;
static const int POST_ROW_H = 28, POST_ROWS = 9;

static void postDraw(bool full) {
  static const uint16_t col[4] = {GOOD, INFO, WARN, ALERT};
  if (full) {
    tft.fillScreen(BG);
    widgets::header("POST", true);
    widgets::footer("UP/DOWN scroll  hold LEFT back");
  }
  tft.fillRect(0, HEADER_H + 2, W, POST_ROWS * POST_ROW_H, BG);
  for (int r = 0; r < POST_ROWS; r++) {
    int i = postTop + r;
    if (i >= post::count()) break;
    const post::Item& it = post::item(i);
    int y = HEADER_H + 4 + r * POST_ROW_H;
    tft.setTextSize(1);
    tft.setTextColor(col[it.level], BG);
    tft.drawString(post::levelName(it.level), 8, y + 2);
    tft.setTextColor(FG, BG);
    tft.drawString(it.name, 44, y + 2);
    tft.setTextColor(DIM, BG);
    tft.drawString(it.detail, 44, y + 14);
  }
}
static void postKey(Key k) {
  int maxTop = max(0, post::count() - POST_ROWS);
  if (k == Key::Up && postTop > 0) { postTop--; ui::dirty(); }
  else if (k == Key::Down && postTop < maxTop) { postTop++; ui::dirty(); }
}

// ---------------------------------------------------------------- TOUCH CALIBRATION

// Tap 4 crosshairs, then test with the new values before keeping them. Not kept within 20 s
// = the old calibration comes back, so a bad result can never lock you out.
static const int16_t TC_PX[4] = {24, 216, 216, 24}, TC_PY[4] = {24, 24, 296, 296};
static int32_t tcRawX[4], tcRawY[4];
static int tcStep = 0;              // 0-3 targets, 4 testing
static TouchCal tcOld;
static bool tcKept = false;
static uint32_t tcTestUntil = 0;
static const char* tcMsg = nullptr;   // shown instead of the step text after a failed try
static const Box TC_KEEP = {10, 250, 105, 44}, TC_REDO = {125, 250, 105, 44};

static void tcCross(int x, int y, uint16_t c) {
  tft.drawFastHLine(x - 12, y, 25, c);
  tft.drawFastVLine(x, y - 12, 25, c);
  tft.drawCircle(x, y, 6, c);
}
static void tcCenter(const char* a, const char* b, uint16_t c) {
  tft.fillRect(0, 110, W, 80, BG);
  tft.setTextDatum(textdatum_t::middle_center);
  tft.setTextColor(c, BG);
  tft.setTextSize(2);
  tft.drawString(a, W / 2, 130);
  tft.setTextSize(1);
  tft.setTextColor(DIM, BG);
  tft.drawString(b, W / 2, 160);
  tft.setTextDatum(textdatum_t::top_left);
}
static void tcEnter() {
  tcOld = input::touchCal();
  tcStep = 0;
  tcKept = false;
  tcMsg = nullptr;
}
static void tcLeave() {
  if (!tcKept) input::setTouchCal(tcOld, false);
}
static void tcDraw(bool full) {
  if (!full) {   // testing countdown
    if (tcStep != 4) return;
    char b[24];
    snprintf(b, sizeof(b), "undo in %lus", (unsigned long)((tcTestUntil - millis() + 999) / 1000));
    tft.fillRect(0, 300, W, 20, BG);
    tft.setTextDatum(textdatum_t::middle_center);
    tft.setTextColor(DIM, BG);
    tft.setTextSize(1);
    tft.drawString(b, W / 2, 310);
    tft.setTextDatum(textdatum_t::top_left);
    return;
  }
  tft.fillScreen(BG);
  if (tcStep < 4) {
    tcCross(TC_PX[tcStep], TC_PY[tcStep], FG);
    char a[24];
    snprintf(a, sizeof(a), "Tap the + (%d/4)", tcStep + 1);
    tcCenter(tcMsg && tcStep == 0 ? tcMsg : a, "hold LEFT = cancel   RIGHT = default", tcMsg && tcStep == 0 ? WARN : FG);
  } else {
    tcCenter("Tap around to test", "dots should land under your finger", FG);
    tft.setTextSize(2);
    ckButton(TC_KEEP, "KEEP", false);
    ckButton(TC_REDO, "REDO", false);
    tft.setTextSize(1);
    tft.setTextColor(DIM, BG);
    tft.setTextDatum(textdatum_t::middle_center);
    tft.drawString("CENTER = keep   LEFT = redo", W / 2, 236);
    tft.setTextDatum(textdatum_t::top_left);
    tcDraw(false);
  }
}
static void tcRestart(const char* why) {
  input::setTouchCal(tcOld, false);
  tcStep = 0;
  tcMsg = why;
  ui::redraw();
}
static void tcKeep() {
  input::setTouchCal(input::touchCal(), true);
  tcKept = true;
  ui::back();
  ui::notify("touch calibrated");
}
static void tcFinish() {
  // Average the two readings per edge, then extend to the screen edges.
  int32_t xa = (tcRawX[0] + tcRawX[3]) / 2, xb = (tcRawX[1] + tcRawX[2]) / 2;
  int32_t ya = (tcRawY[0] + tcRawY[1]) / 2, yb = (tcRawY[2] + tcRawY[3]) / 2;
  bool square = abs(tcRawX[0] - tcRawX[3]) < 800 && abs(tcRawX[1] - tcRawX[2]) < 800 &&
                abs(tcRawY[0] - tcRawY[1]) < 800 && abs(tcRawY[2] - tcRawY[3]) < 800;
  if (!square || abs(xb - xa) < 1200 || abs(yb - ya) < 1200) {
    sio::out("[touchcal] rejected x %ld %ld y %ld %ld", (long)xa, (long)xb, (long)ya, (long)yb);
    tcRestart("Missed. Try again");
    return;
  }
  TouchCal c;
  c.xl = xa - 24 * (xb - xa) / 192;
  c.xr = xa + 216 * (xb - xa) / 192;
  c.yt = ya - 24 * (yb - ya) / 272;
  c.yb = ya + 296 * (yb - ya) / 272;
  sio::out("[touchcal] x %d..%d  y %d..%d", c.xl, c.xr, c.yt, c.yb);
  input::setTouchCal(c, false);   // try it, not saved yet
  tcStep = 4;
  tcTestUntil = millis() + 20000;
  ui::redraw();
}
static void tcTap(int x, int y) {
  if (tcStep < 4) {
    int32_t rx, ry;
    if (!input::lastRaw(rx, ry)) return;
    tcRawX[tcStep] = rx;
    tcRawY[tcStep] = ry;
    if (++tcStep == 4) tcFinish();
    else ui::redraw();
    return;
  }
  if (inBox(TC_KEEP, x, y)) { tcKeep(); return; }
  if (inBox(TC_REDO, x, y)) { tcRestart(nullptr); return; }
  tft.fillCircle(x, y, 3, GOOD);
  tcTestUntil = millis() + 20000;   // still testing, keep the timer going
}
static void tcKey(Key k) {
  if (tcStep < 4) {
    if (k == Key::Right) {
      input::resetTouchCal();
      tcKept = true;   // already saved
      ui::back();
      ui::notify("touch: factory default");
    }
    return;
  }
  if (k == Key::Center) tcKeep();
  else if (k == Key::Left) tcRestart(nullptr);
}
static void tcTick() {
  if (tcStep != 4) return;
  if ((int32_t)(millis() - tcTestUntil) >= 0) {
    tcRestart("Not kept. Undone");
    return;
  }
  static uint32_t lastSec = 0;
  if (millis() / 1000 != lastSec) { lastSec = millis() / 1000; ui::dirty(); }
}

// ---------------------------------------------------------------- BUTTON TEST

// Shows the D-pad live from the PCF8574, straight from the chip (no debounce or key
// mapping), so a dead or crossed button is plain to see. Only the 5 D-pad pins are watched.
struct BtDef { const char* name; uint8_t bit; Box box; };
static const BtDef BT[5] = {
    {"UP", PCF_BIT_UP, {85, 60, 70, 44}},        {"LEFT", PCF_BIT_LEFT, {8, 112, 72, 44}},
    {"CENTER", PCF_BIT_SELECT, {85, 112, 70, 44}}, {"RIGHT", PCF_BIT_RIGHT, {160, 112, 72, 44}},
    {"DOWN", PCF_BIT_DOWN, {85, 164, 70, 44}}};
static const uint8_t BT_DPAD = (1 << PCF_BIT_UP) | (1 << PCF_BIT_DOWN) | (1 << PCF_BIT_LEFT) |
                               (1 << PCF_BIT_RIGHT) | (1 << PCF_BIT_SELECT);
static uint8_t btSeen;     // bit set = that button has been seen pressed
static int btShown;        // D-pad state last drawn (bits set = held), -2 = redraw, -1 = no chip
static int btState() {
  int v = input::rawButtons();
  return v < 0 ? -1 : (~v & BT_DPAD);
}
static void btEnter() { btSeen = 0; btShown = -2; }
static void btDraw(bool full) {
  int st = btState();
  if (full) {
    tft.fillScreen(BG);
    widgets::header("BUTTON TEST", true);
    widgets::footer("hold LEFT or tap BACK");
    btShown = -2;
  }
  if (st == btShown) return;
  uint8_t down = st < 0 ? 0 : (uint8_t)st;
  btSeen |= down;
  tft.setTextSize(1);
  tft.setTextDatum(textdatum_t::middle_center);
  for (auto& b : BT) {
    bool held = down & (1 << b.bit), seen = btSeen & (1 << b.bit);
    uint16_t c = held ? FG : seen ? GOOD : DIM;
    tft.fillRoundRect(b.box.x, b.box.y, b.box.w, b.box.h, 6, held ? FG : BG);
    tft.drawRoundRect(b.box.x, b.box.y, b.box.w, b.box.h, 6, c);
    tft.setTextColor(held ? BG : c, held ? FG : BG);
    tft.drawString(b.name, b.box.x + b.box.w / 2, b.box.y + b.box.h / 2);
  }
  tft.fillRect(0, 226, W, 20, BG);
  tft.setTextColor(st < 0 ? ALERT : btSeen == BT_DPAD ? GOOD : FG, BG);
  tft.drawString(st < 0 ? "Button chip not answering" : btSeen == BT_DPAD ? "All 5 buttons work" : "Press each button",
                 W / 2, 236);
  tft.setTextDatum(textdatum_t::top_left);
  btShown = st;
}
static void btTick() {
  if (btState() != btShown) ui::dirty();
}

// ---------------------------------------------------------------- About

static void aboutDraw(bool full) {
  if (!full) return;
  tft.fillScreen(BG);
  widgets::header("ABOUT", true);
  drawWrapped("FUD-DECK " FUD_VERSION "\n"
              "Runs on ESP32-DIV V2 hardware. Not related to the ESP32-DIV firmware.\n\n"
              "Education and monitoring. FUD-Deck listens and explains. No attack modes are enabled by default.\n\n"
              "A branch of FUD by thoughtfix, like FUD-X3. Zuse, faces, alert tiers and moods "
              "from FUD. SquachWatch-CYD (Talking Sasquach) does similar work with the ESP32's "
              "own radios; FUD adds more. Some camera signatures come from its research.\n"
              "Libraries: LovyanGFX, NimBLE-Arduino, IRremoteESP8266.\n\n"
              "Board notes: the third nRF24 is wired to the IR LED driver on V2 boards and cannot "
              "receive, so FUD-Deck uses two. POST checks this on every boot.",
              8, HEADER_H + 12, W - 16, FG);
  widgets::footer("hold LEFT or tap BACK");
}

// ---------------------------------------------------------------- table

static void loadSettings() {
  static bool loaded = false;
  if (loaded) return;
  loaded = true;
  prefs.begin("fud", false);
  leds::setLevel(prefs.getUChar("led", 1));
  leds::clear();
  saverOn = prefs.getBool("saver", false);
  applySaver();
  sleepIdx = prefs.getUChar("sleep", 2) % 5;
  ui::setSleepAfter(SLEEP_MS[sleepIdx]);
  det::hooks.passive = onPassive;
  det::hooks.alarm = onAlarm;
  det::hooks.follow = onFollow;
}

static void homeEnter() { loadSettings(); homeFace = nullptr; animKind = ANIM_NONE; animSchedule(); }

const Screen SCREENS[] = {
  {"home", homeEnter, homeDraw, homeKey, homeTap, homeTick},
  {"menu", menuEnter, menuDraw, menuKey, menuTap, nullptr},
  {"log", logEnter, logDraw, logKey, logTap, logTick},
  {"bundle", bundleEnter, logDraw, logKey, logTap, logTick},
  {"item", itemEnter, itemDraw, itemKey, itemTap, nullptr},
  {"attack", attackEnter, attackDraw, attackKey, attackTap, nullptr},
  {"groups", grpEnter, grpDraw, grpKey, grpTap, grpTick},
  {"toys", toysEnter, toysDraw, toysKey, toysTap, nullptr},
  {"toy", nullptr, stubDraw, nullptr, nullptr, nullptr},
  {"waterfall", toys::wfEnter, toys::wfDraw, toys::wfKey, toys::wfTap, toys::wfTick, toys::wfLeave},
  {"traffic", toys::trEnter, toys::trDraw, toys::trKey, toys::trTap, toys::trTick, toys::trLeave},
  {"ssidscan", toys::ssEnter, toys::ssDraw, toys::ssKey, toys::ssTap, toys::ssTick, toys::ssLeave},
  {"ssiddetail", toys::sdEnter, toys::sdDraw, toys::sdKey, toys::sdTap, toys::sdTick, nullptr},
  {"irdecode", toys::irEnter, toys::irDraw, toys::irKey, toys::irTap, toys::irTick, toys::irLeave},
  {"tvbgone", toys::tvEnter, toys::tvDraw, toys::tvKey, toys::tvTap, toys::tvTick, toys::tvLeave},
  {"finder", toys::fdEnter, toys::fdDraw, toys::fdKey, toys::fdTap, toys::fdTick, nullptr},
  {"finderlock", toys::flEnter, toys::flDraw, toys::flKey, toys::flTap, toys::flTick, toys::flLeave},
  {"settings", setEnter, setDraw, setKey, setTap, nullptr},
  {"setclock", ckEnter, ckDraw, ckKey, ckTap, nullptr},
  {"post", nullptr, postDraw, postKey, nullptr, nullptr},
  {"about", nullptr, aboutDraw, nullptr, nullptr, nullptr},
  {"touchcal", tcEnter, tcDraw, tcKey, tcTap, tcTick, tcLeave},
  {"buttons", btEnter, btDraw, nullptr, nullptr, btTick},
};
static_assert(sizeof(SCREENS) / sizeof(SCREENS[0]) == (int)ScreenId::COUNT, "screen table out of sync");
