#include "devtools.h"
#include "../board/power.h"
#include <Arduino.h>
#include "../board/display.h"
#include "../board/input.h"
#include "../board/post.h"
#include "../board/serial_io.h"
#include "../core/detections.h"
#include "../core/scanner.h"
#include "../core/sdlog.h"
#include "../core/clock.h"
#include "../core/zuse.h"
#include "../ui/ui.h"
#include "../ui/toys.h"
#include "../ui/face.h"
#include "../ui/facevec.h"

void homeAnimNow(int kind);   // screens.cpp: idle animation on Home now

namespace devtools {

#ifdef FUD_DEVTOOLS

static void shot() {
  int w = tft.width(), h = tft.height();
  static lgfx::rgb565_t row[320];
  static char hex[2 * 320 + 16];
  static const char* hx = "0123456789ABCDEF";
  sio::setReliable(true);
  sio::out("[shot] BEGIN %d %d %d", w, h, (int)ui::current());
  for (int y = 0; y < h; y++) {
    tft.readRect(0, y, w, 1, row);
    int n = snprintf(hex, sizeof(hex), "R%d:", y);
    for (int x = 0; x < w; x++) {
      uint8_t v = (row[x].R8() & 0xE0) | ((row[x].G8() & 0xE0) >> 3) | (row[x].B8() >> 6);
      hex[n++] = hx[v >> 4];
      hex[n++] = hx[v & 15];
    }
    hex[n] = 0;
    sio::raw(hex);
  }
  sio::out("[shot] END");
  sio::setReliable(false);
}

// Inject a made-up detection so alert paths can be tested without radios.
//   fake tracker|camera|drone|subghz|hackdev|attack|jam [key-byte]
static void fake(const String& arg) {
  struct F { const char* n; det::Cat c; const char* label; const char* name; char src; bool active; };
  static const F T[] = {
    {"tracker", det::TRACKER, "AirTag", "AirTag", 'B', false},
    {"camera", det::CAMERA, "Flock ALPR", "Flock Safety", 'B', false},
    {"drone", det::DRONE, "Remote ID", "DJI Mini", 'B', false},
    {"subghz", det::SUBGHZ, "433 burst", "433.92 MHz", 'S', false},
    {"hackdev", det::HACKDEV, "Flipper", "Flipper Zero", 'B', true},
    {"attack", det::ATTACK, "Deauth", "Deauth flood", 'W', true},
    {"jam", det::ATTACK, "Jammer", "2.4 GHz jammer", 'N', true},
  };
  int sp = arg.indexOf(' ');
  String what = sp < 0 ? arg : arg.substring(0, sp);
  uint8_t kb = sp < 0 ? (uint8_t)esp_random() : (uint8_t)arg.substring(sp + 1).toInt();
  for (auto& f : T) {
    if (what != f.n) continue;
    uint8_t key[6] = {0xFA, 0x4E, (uint8_t)f.c, 0x00, 0x00, kb};
    det::enq(key, f.c, f.label, f.name, -40 - (kb % 40), f.src, false, f.active, 'H');
    sio::out("[dev] fake %s key %02X", f.n, kb);
    return;
  }
  sio::out("[dev] fake what? tracker camera drone subghz hackdev attack jam");
}

static Key parseKey(const String& s) {
  if (s == "up") return Key::Up;
  if (s == "down") return Key::Down;
  if (s == "left") return Key::Left;
  if (s == "right") return Key::Right;
  if (s == "center" || s == "select") return Key::Center;
  if (s == "back") return Key::Back;
  if (s == "holdcenter") return Key::HoldCenter;
  return Key::None;
}

void poll() {
  String line;
  if (!sio::readLine(line)) return;
  int sp = line.indexOf(' ');
  String cmd = sp < 0 ? line : line.substring(0, sp);
  String raw = sp < 0 ? "" : line.substring(sp + 1);   // case kept, for "say"
  String arg = raw;
  cmd.toLowerCase();
  arg.toLowerCase();
  if (cmd == "key") {
    Key k = parseKey(arg);
    if (k == Key::None) sio::out("[dev] unknown key '%s'", arg.c_str());
    else input::inject(k);
  } else if (cmd == "tap") {
    int sp2 = arg.indexOf(' ');
    input::injectTap(arg.substring(0, sp2).toInt(), arg.substring(sp2 + 1).toInt());
  } else if (cmd == "swipe") {   // swipe up|down|right
    if (arg == "up") input::injectSwipe(120, 250, 0, -120);
    else if (arg == "down") input::injectSwipe(120, 100, 0, 120);
    else if (arg == "right") input::injectSwipe(40, 160, 140, 0);
  } else if (cmd == "toast") {   // banner on Home: toast <text>
    static char t[48];
    snprintf(t, sizeof(t), "%s", raw.c_str());
    ui::notify(t);
  } else if (cmd == "fake") {
    fake(arg);
  } else if (cmd == "pause") {
    scanner::pause(arg == "on");
    sio::out("[dev] scanner pause %s, heap now %lu", arg.c_str(), (unsigned long)ESP.getFreeHeap());
  } else if (cmd == "time") {   // time <epoch> [zone-index]
    int sp2 = arg.indexOf(' ');
    if (sp2 > 0) clk::setZone(arg.substring(sp2 + 1).toInt());
    bool ok = clk::setEpoch(strtoul(arg.c_str(), nullptr, 10));
    char b[24];
    clk::formatDateTime(b, sizeof(b));
    sio::out("[dev] time %s: %s %s", ok ? "set" : "rejected", b, clk::zoneName(clk::zone()));
  } else if (cmd == "face") {   // show a face on Home for 20 s: face <name>
    zuse::react(strdup(arg.c_str()), 20000);   // leaks a few bytes per call, dev only
    ui::home();
    sio::out("[dev] face %s", arg.c_str());
  } else if (cmd == "facebench") {   // time every face drawn at 162 px on the panel
    static const char* EXTRA[] = {"camera", "alarm-attack", "alarm-hacker"};
    uint32_t total = 0;
    int n = facevec::count() + 3;
    for (int i = 0; i < n; i++) {
      const char* nm = i < facevec::count() ? facevec::name(i) : EXTRA[i - facevec::count()];
      uint32_t t0 = micros();
      face::draw(nm, 39, 32, 162, 0x0000);
      uint32_t us = micros() - t0;
      total += us;
      sio::out("[dev] facebench %-16s %5lu us", nm, (unsigned long)us);
    }
    sio::out("[dev] facebench avg %lu us", (unsigned long)(total / n));
    ui::home();
  } else if (cmd == "go") {   // open a screen by its table name: go buttons
    for (int i = 0; i < (int)ScreenId::COUNT; i++)
      if (arg == SCREENS[i].name) { ui::go((ScreenId)i); break; }
  } else if (cmd == "anim") {   // idle animation on Home now: anim spin|sweep
    ui::home();
    homeAnimNow(arg == "spin" ? 1 : arg == "sweep" ? 2 : 0);
  } else if (cmd == "say") {   // speech bubble on Home for 20 s: say <text>
    zuse::say(raw.c_str(), 20000);
    ui::home();
    sio::out("[dev] say %s", raw.c_str());
  } else if (cmd == "irtest") {   // fake decoded NEC frame on the IR Decoder screen
    toys::irFake();
  } else if (cmd == "sdformat") {   // needs "sdformat yes": erases the card
    if (arg == "yes") sdlog::formatIfUnreadable();
    else sio::out("[dev] sdformat erases the card. Type: sdformat yes");
  } else if (cmd == "sdrm") {     // sdrm 20261111.csv
    sdlog::removeFile(arg.c_str());
  } else if (cmd == "sdhead") {   // sdhead 20261111.csv [lines]
    int sp2 = arg.indexOf(' ');
    String f = sp2 < 0 ? arg : arg.substring(0, sp2);
    sdlog::head(f.c_str(), sp2 < 0 ? 4 : arg.substring(sp2 + 1).toInt());
  } else if (cmd == "sdbench") {
    int sp2 = arg.indexOf(' ');   // sdbench [mhz] [rounds] [gdo|ccoff|nrfoff|alloff]
    String rest = sp2 > 0 ? arg.substring(sp2 + 1) : "";
    int sp3 = rest.indexOf(' ');
    String quiet = sp3 > 0 ? rest.substring(sp3 + 1) : "";
    sdlog::bench(arg.toInt(), rest.length() ? rest.toInt() : 1, quiet.c_str());
  } else if (cmd == "reboot") {
    sio::out("[dev] rebooting");
    delay(100);
    ESP.restart();
  } else if (cmd == "sdtail") {
    sdlog::tail(arg.length() ? arg.toInt() : 8);
  } else if (cmd == "ipregs") {
    power::dumpRegs();
  } else if (cmd == "heap") {
    sio::out("[dev] heap %lu  min ever %lu  largest block %lu", (unsigned long)ESP.getFreeHeap(),
             (unsigned long)ESP.getMinFreeHeap(), (unsigned long)ESP.getMaxAllocHeap());
  } else if (cmd == "shot") {
    shot();
  } else if (cmd == "state") {
    sio::out("[dev] screen %s", ui::currentName());
  } else if (cmd == "post") {
    for (int i = 0; i < post::count(); i++) {
      const post::Item& it = post::item(i);
      sio::out("[post] %-8s %-4s %s", it.name, post::levelName(it.level), it.detail);
    }
  } else {
    sio::out("[dev] commands: key <up|down|left|right|center|back|holdcenter>, tap X Y, shot, state, post, fake <kind>");
  }
  sio::out("[dev] DONE %s", cmd.c_str());
}

#else
void poll() {}
#endif

}  // namespace devtools
