#include "post.h"
#include <SD.h>
#include <esp_chip_info.h>
#include "bus.h"
#include "input.h"
#include "pins.h"
#include "power.h"
#include "serial_io.h"
#include <Wire.h>

namespace post {

static const int MAX_ITEMS = 12;
static Item items[MAX_ITEMS];
static int n = 0;
static bool okButtons = false, okSd = false, okCc = false, okBuzz = false;
static bool okNrf[3] = {false, false, false};
static void (*cb)(int, const Item&) = nullptr;

static void add(const char* name, Level lvl, const char* fmt, ...) {
  if (n >= MAX_ITEMS) return;
  Item& it = items[n];
  it.name = name;
  it.level = lvl;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(it.detail, sizeof(it.detail), fmt, ap);
  va_end(ap);
  sio::out("[post] %-8s %-4s %s", name, levelName(lvl), it.detail);
  if (cb) cb(n, it);
  n++;
}

// ---- raw SPI probes (no radio libraries needed to say "present") ----

static const SPISettings PROBE_SPI(4000000, MSBFIRST, SPI_MODE0);

// nRF24: R_REGISTER SETUP_AW. Status comes back on the command byte. Healthy: 0x0E, AW 1..3.
static bool nrfProbe(uint8_t csn, uint8_t* status, uint8_t* aw) {
  bus::deselectAll();
  SPI.beginTransaction(PROBE_SPI);
  digitalWrite(csn, LOW);
  *status = SPI.transfer(0x03);
  *aw = SPI.transfer(0xFF);
  digitalWrite(csn, HIGH);
  SPI.endTransaction();
  return *status != 0xFF && *aw >= 1 && *aw <= 3;
}

// CC1101: chip pulls SO low when ready after CS goes low. Then read VERSION (0x31).
static int ccVersion() {
  bus::deselectAll();
  SPI.beginTransaction(PROBE_SPI);
  digitalWrite(PIN_CC_CS, LOW);
  uint32_t t = micros();
  bool ready = true;
  while (digitalRead(PIN_SPI_MISO)) {
    if (micros() - t > 3000) { ready = false; break; }
  }
  int v = -1;
  if (ready) {
    SPI.transfer(0x31 | 0xC0);
    v = SPI.transfer(0);
  }
  digitalWrite(PIN_CC_CS, HIGH);
  SPI.endTransaction();
  return v;
}

static void checkSd() {
  pinMode(PIN_SD_CD, INPUT_PULLUP);
  bool inserted = digitalRead(PIN_SD_CD) == 0;  // measured: 0 = card in
  if (!inserted) {
    add("SD card", INFO, "no card");
    return;
  }
  bus::deselectAll();
  bool mounted = SD.begin(PIN_SD_CS, SPI, SD_HZ); bus::setDrive();
  if (!mounted) {
    // A card left mid-transaction by a reset often fails its first init. Release it, retry.
    SD.end();
    bus::sdRelease();
    delay(50);
    mounted = SD.begin(PIN_SD_CS, SPI, SD_HZ); bus::setDrive();
  }
  bool rw = false, opened = false;
  size_t put = 0;
  int got = -1;
  uint64_t mb = 0;
  if (mounted) {
    mb = SD.cardSize() / (1024 * 1024);
    // Same kind of write the logger does, in the logger's folder: append, close, check size.
    if (!SD.exists("/.fuddeck")) SD.mkdir("/.fuddeck");
    for (int attempt = 0; attempt < 2 && !rw; attempt++) {
      SD.remove("/.fuddeck/post.tmp");
      File f = SD.open("/.fuddeck/post.tmp", FILE_APPEND);
      opened = (bool)f;
      static const char MARK[] = "fud";
      put = f ? f.print(MARK) : 0;
      if (f) f.close();
      f = SD.open("/.fuddeck/post.tmp", FILE_READ);
      got = f ? (int)f.size() : -1;
      if (f) f.close();
      rw = put == (int)strlen(MARK) && got == (int)strlen(MARK);
      if (!rw && attempt == 0) {
        // Seen on hardware: the first mount after a reset can come up unable to open files.
        // A clean unmount and remount fixes it.
        SD.end();
        bus::sdRelease();
        delay(50);
        if (!SD.begin(PIN_SD_CS, SPI, SD_HZ)) break;
        if (!SD.exists("/.fuddeck")) SD.mkdir("/.fuddeck");
      }
    }
    SD.remove("/.fuddeck/post.tmp");
    SD.end();
  }
  bus::sdRelease();
  okSd = mounted && rw;
  if (!mounted) add("SD card", WARN, "will not mount. FAT32?");
  else if (okSd) add("SD card", OK, "%lluMB read/write ok", mb);
  else add("SD card", WARN, "%lluMB write fail o%d p%u s%d", mb, opened, (unsigned)put, got);
}

void run(void (*onItem)(int, const Item&)) {
  cb = onItem;
  n = 0;

  okButtons = input::buttonsPresent();
  add("Buttons", okButtons ? OK : FAIL, okButtons ? "PCF8574 0x20" : "PCF8574 missing");

  if (power::method() == power::Method::None)
    add("Battery", WARN, "no method (IO2 clamped, no IP5306)");
  else
    add("Battery", OK, "%s %d%%%s", power::methodName(), power::percent(),
        power::boostKeepOn() == 1 ? ", stays on" : power::boostKeepOn() == 0 ? ", may auto-off" : "");

  checkSd();

  // Radios after SD on purpose: proves the card let go of the bus.
  const uint8_t csn[3] = {PIN_NRF1_CSN, PIN_NRF2_CSN, PIN_NRF3_CSN};
  static const char* names[3] = {"nRF24 1", "nRF24 2", "nRF24 3"};
  bool ce3 = false;
  int held = 0;
  for (int i = 0; i < 3; i++) {
    if (i == 2) bus::setNrf3Usable(true);   // CE checked below; CSN may be driven for the probe
    if (i == 2 && !(ce3 = bus::nrf3CeWorks())) {
      bus::setNrf3Usable(false);
      okNrf[2] = false;
      add(names[2], INFO, "off: CE clamped (normal on V2)");
      continue;
    }
    uint8_t st = 0, aw = 0;
    okNrf[i] = nrfProbe(csn[i], &st, &aw);
    if (!okNrf[i] && st == 0xFF && okSd) {
      // Some cards keep driving MISO after release. One more release, then retry.
      bus::sdRelease();
      delay(5);
      okNrf[i] = nrfProbe(csn[i], &st, &aw);
    }
    if (!okNrf[i] && st == 0xFF) held++;
    if (i == 2) bus::setNrf3Usable(okNrf[2]);
    if (okNrf[i]) add(names[i], OK, "status 0x%02X", st);
    else if (st == 0xFF && okSd) add(names[i], FAIL, "no answer (bus held high?)");
    else add(names[i], FAIL, "no answer (0x%02X/0x%02X)", st, aw);
  }

  int v = ccVersion();
  okCc = v == 0x14 || v == 0x04;
  if (okCc) add("CC1101", OK, "version 0x%02X", v);
  else add("CC1101", FAIL, v < 0 ? "not ready" : "bad version 0x%02X", v);
  // Every radio silent with a working card: seen with a Lexar card that never lets go of
  // MISO. (Also what an unplugged shield looks like.) Say so instead of blaming the radios.
  if (okSd && held && !okNrf[0] && !okNrf[1] && !okCc)
    add("SD bus", WARN, "card may hold the bus. Try another card");

  Wire.beginTransmission(0x1E);
  okBuzz = Wire.endTransmission() == 0;
  add("Buzzer", INFO, okBuzz ? "Modulino 0x1E" : "none (QWIIC add-on)");

  add("PSRAM", INFO, "unused: shares TFT pins 35-37");

  bus::deselectAll();
  cb = nullptr;
}

int count() { return n; }
const Item& item(int i) { return items[i]; }
const char* levelName(Level l) {
  static const char* s[4] = {"OK", "INFO", "WARN", "FAIL"};
  return s[l & 3];
}
bool buttons() { return okButtons; }
bool sd() { return okSd; }
bool nrf(int i) { return i >= 0 && i < 3 && okNrf[i]; }
int nrfCount() { return okNrf[0] + okNrf[1] + okNrf[2]; }
bool cc1101() { return okCc; }
bool buzzer() { return okBuzz; }

}  // namespace post
