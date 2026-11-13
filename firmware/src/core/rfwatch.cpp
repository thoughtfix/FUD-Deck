#include "rfwatch.h"
#include <SPI.h>
#include "../board/bus.h"
#include "../board/pins.h"
#include "../board/post.h"
#include "../board/serial_io.h"
#include "detections.h"

namespace rfwatch {

static const SPISettings NRF_SPI(8000000, MSBFIRST, SPI_MODE0);
static const SPISettings CC_SPI(4000000, MSBFIRST, SPI_MODE0);
static const int CH = 126;

struct Nrf { uint8_t ce, csn; int lo, hi; };   // segment [lo, hi)
static Nrf nrf[3];
static int nN = 0;
static bool cc = false;
static volatile bool saver = false;

static uint8_t hotNow[CH];
static volatile int busy = 0;
static volatile int ccLast = -120, ccNoise = -100;
static volatile uint32_t nSweeps = 0;
static int segP[3] = {0, 0, 0};
static portMUX_TYPE accMux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t acc[CH];
static uint16_t accN = 0;
static volatile bool ccSweepReq = false;
static bool ccSweeping = false;
static int8_t ccBins[CC_BINS];
static volatile uint32_t ccSeq = 0;

int nrfRadios() { return nN; }
bool ccActive() { return cc; }
void setSaver(bool on) { saver = on; }
int busyPct() { return busy; }
void lastSweep(uint8_t* out) { memcpy(out, hotNow, CH); }
int ccRssi() { return ccLast; }
int ccFloor() { return ccNoise; }
uint32_t sweeps() { return nSweeps; }
int segPct(int i) { return i >= 0 && i < 3 ? segP[i] : 0; }
void setCcSweep(bool on) { ccSweepReq = on; }
static volatile Focus focusReq = Focus::All;
void setFocus(Focus f) { focusReq = f; }

void grabCounts(uint8_t* out, int& n) {
  portENTER_CRITICAL(&accMux);
  memcpy(out, acc, CH);
  n = accN;
  memset(acc, 0, CH);
  accN = 0;
  portEXIT_CRITICAL(&accMux);
}

uint32_t ccRow(int8_t* out) {
  portENTER_CRITICAL(&accMux);
  memcpy(out, ccBins, CC_BINS);
  uint32_t s = ccSeq;
  portEXIT_CRITICAL(&accMux);
  return s;
}

// BLE advertising channels 37/38/39 = 2402/2426/2480 MHz, +-1 channel.
static bool masked(int ch) {
  return (ch >= 1 && ch <= 3) || (ch >= 25 && ch <= 27) || (ch >= 79 && ch <= 81);
}

// ---------------------------------------------------------------- nRF24 raw access

static uint8_t nrfRead(const Nrf& r, uint8_t reg) {
  digitalWrite(r.csn, LOW);
  SPI.transfer(reg & 0x1F);
  uint8_t v = SPI.transfer(0xFF);
  digitalWrite(r.csn, HIGH);
  return v;
}
static void nrfWrite(const Nrf& r, uint8_t reg, uint8_t v) {
  digitalWrite(r.csn, LOW);
  SPI.transfer(0x20 | (reg & 0x1F));
  SPI.transfer(v);
  digitalWrite(r.csn, HIGH);
}

static void nrfSetup() {
  SPI.beginTransaction(NRF_SPI);
  for (int i = 0; i < nN; i++) {
    digitalWrite(nrf[i].ce, LOW);
    nrfWrite(nrf[i], 0x01, 0x00);   // EN_AA off
    nrfWrite(nrf[i], 0x00, 0x0B);   // PWR_UP, PRIM_RX, CRC
  }
  SPI.endTransaction();
  delay(3);                          // power-up to standby
}

// One snapshot of the whole band: all radios step through their segments together.
static void nrfSweep(int segHot[3], int segLen[3]) {
  int maxLen = 0;
  for (int i = 0; i < nN; i++) {
    segHot[i] = 0;
    segLen[i] = 0;
    maxLen = max(maxLen, nrf[i].hi - nrf[i].lo);
  }
  SPI.beginTransaction(NRF_SPI);
  for (int step = 0; step < maxLen; step++) {
    for (int i = 0; i < nN; i++) {
      int ch = nrf[i].lo + step;
      if (ch < nrf[i].hi) nrfWrite(nrf[i], 0x05, ch);
    }
    for (int i = 0; i < nN; i++) digitalWrite(nrf[i].ce, HIGH);
    delayMicroseconds(170);          // 130 us settle + RPD window
    for (int i = 0; i < nN; i++) digitalWrite(nrf[i].ce, LOW);
    for (int i = 0; i < nN; i++) {
      int ch = nrf[i].lo + step;
      if (ch >= nrf[i].hi) continue;
      bool hot = nrfRead(nrf[i], 0x09) & 0x01;
      hotNow[ch] = hot;
      if (masked(ch)) continue;
      segLen[i]++;
      if (hot) segHot[i]++;
    }
  }
  SPI.endTransaction();
}

// ---------------------------------------------------------------- CC1101 raw access

static bool ccSelect() {
  digitalWrite(PIN_CC_CS, LOW);
  uint32_t t = micros();
  while (digitalRead(PIN_SPI_MISO))
    if (micros() - t > 3000) return false;
  return true;
}
static void ccStrobe(uint8_t s) { ccSelect(); SPI.transfer(s); digitalWrite(PIN_CC_CS, HIGH); }
static void ccWrite(uint8_t a, uint8_t v) { ccSelect(); SPI.transfer(a); SPI.transfer(v); digitalWrite(PIN_CC_CS, HIGH); }
static uint8_t ccStatus(uint8_t a) {
  ccSelect();
  SPI.transfer(0xC0 | a);
  uint8_t v = SPI.transfer(0);
  digitalWrite(PIN_CC_CS, HIGH);
  return v;
}

// 433.92 MHz, widest bandwidth, OOK, no sync: pure carrier sense. From FUD.
static void ccInit433() {
  SPI.beginTransaction(CC_SPI);
  ccStrobe(0x30);
  delay(1);
  ccWrite(0x0D, 0x10); ccWrite(0x0E, 0xB0); ccWrite(0x0F, 0x71);
  ccWrite(0x10, 0x07);
  ccWrite(0x12, 0x30);
  ccWrite(0x0B, 0x06);
  // Async serial mode: received bits go to the GDO pins, never into the RX FIFO. With no
  // sync word the FIFO would fill with noise, overflow, and freeze RSSI after a few seconds.
  ccWrite(0x08, 0x32);
  ccStrobe(0x33);
  delay(1);
  ccStrobe(0x34);   // SRX and stay there
  SPI.endTransaction();
  delay(2);
}

static int ccSample() {
  SPI.beginTransaction(CC_SPI);
  uint8_t r = ccStatus(0x34);
  uint8_t marc = ccStatus(0x35) & 0x1F;
  if (marc != 0x0D) {                  // fell out of RX: flush and go back in
    ccStrobe(0x36);                    // SIDLE
    ccStrobe(0x3A);                    // SFRX
    ccStrobe(0x34);                    // SRX
  }
  SPI.endTransaction();
  int d = r >= 128 ? (int)r - 256 : r;
  return d / 2 - 74;
}

// ---------------------------------------------------------------- detection logic

static const int JAM_WINDOW = 25;        // sweeps (~1 s at full speed)
static const int JAM_NEED = 18;          // of those
static const int SEG_HOT_PCT = 40;
static const int SUB_STRONG_DBM = -65;   // FUD threshold: a close transmitter
static uint8_t jamHist[JAM_WINDOW];
static int jamPos = 0;

// One step-sweep of 415-450 MHz, strongest of two RSSI reads per bin. From FUD's ccSweepWF.
static void ccSweepOnce() {
  int8_t row[CC_BINS];
  SPI.beginTransaction(CC_SPI);
  if (!ccSweeping) {
    ccStrobe(0x30);
    delay(1);
    ccWrite(0x10, 0x07); ccWrite(0x12, 0x30); ccWrite(0x0B, 0x06); ccWrite(0x08, 0x32);
    ccWrite(0x18, 0x18);   // MCSM0: calibrate on IDLE > RX
    ccSweeping = true;
  }
  for (int i = 0; i < CC_BINS; i++) {
    uint32_t f = CC_LO_HZ + (uint32_t)((uint64_t)(CC_HI_HZ - CC_LO_HZ) * i / CC_BINS);
    uint32_t w = (uint32_t)(((uint64_t)f << 16) / 26000000ULL);
    ccStrobe(0x36);
    ccWrite(0x0D, (w >> 16) & 0xFF); ccWrite(0x0E, (w >> 8) & 0xFF); ccWrite(0x0F, w & 0xFF);
    ccStrobe(0x34);
    delayMicroseconds(1200);   // autocal ~720 us + RSSI settle
    uint8_t r1 = ccStatus(0x34);
    delayMicroseconds(300);
    uint8_t r2 = ccStatus(0x34);
    int d1 = (r1 >= 128 ? (int)r1 - 256 : r1) / 2 - 74, d2 = (r2 >= 128 ? (int)r2 - 256 : r2) / 2 - 74;
    row[i] = (int8_t)max(-127, min(0, max(d1, d2)));
  }
  ccStrobe(0x36);
  SPI.endTransaction();
  portENTER_CRITICAL(&accMux);
  memcpy(ccBins, row, CC_BINS);
  ccSeq++;
  portEXIT_CRITICAL(&accMux);
}

static void judgeJam(const int segHot[3], const int segLen[3]) {
  bool all = nN > 0;
  int pcts[3] = {0, 0, 0};
  for (int i = 0; i < nN; i++) {
    pcts[i] = segLen[i] ? segHot[i] * 100 / segLen[i] : 0;
    if (pcts[i] < SEG_HOT_PCT) all = false;
  }
  jamHist[jamPos] = all;
  jamPos = (jamPos + 1) % JAM_WINDOW;
  int n = 0;
  for (int i = 0; i < JAM_WINDOW; i++) n += jamHist[i];
  if (n >= JAM_NEED) {
    static const uint8_t jkey[6] = {0x32, 0x34, 0x4A, 0x41, 0x4D, 0x00};   // "24JAM"
    char nm[32];
    if (nN >= 2) snprintf(nm, sizeof(nm), "2.4G jam %d%%/%d%%", pcts[0], pcts[1]);
    else snprintf(nm, sizeof(nm), "2.4G jam %d%%", pcts[0]);
    det::enq(jkey, det::ATTACK, "2.4 jammer", nm, -40, 'N', false, true, 'M');
    memset(jamHist, 0, sizeof(jamHist));   // one report per sustained second
  }
}

static int subStrong = 0, subPeak = -120;
static uint32_t subWindowStart = 0;

static int subMin = 0, subMax = -200;
static bool floorInit = false;
static uint32_t floorTick = 0;

static void judgeSub(int dbm) {
  ccLast = dbm;
  if (dbm < subMin) subMin = dbm;
  if (dbm > subMax) subMax = dbm;
  // Noise floor: starts at the first reading, drops quickly to quieter readings, and rises
  // only slowly (1 dB every ~2 s) so bursts never pull it up.
  if (!floorInit) { ccNoise = dbm; floorInit = true; }
  else if (dbm < ccNoise) ccNoise += (dbm - ccNoise - 3) / 4;   // always moves at least 1 dB
  else if (++floorTick % 80 == 0 && dbm < ccNoise + 20) ccNoise = ccNoise + 1;
  if (dbm > SUB_STRONG_DBM) { subStrong++; if (dbm > subPeak) subPeak = dbm; }
  uint32_t now = millis();
  if (now - subWindowStart < 2000) return;
  if (subStrong >= 3) {
    static const uint8_t skey[6] = {0x43, 0x33, 0x00, 0x00, 0x00, 0x00};   // "433"
    char nm[32];
    snprintf(nm, sizeof(nm), "433.92 MHz %ddBm", subPeak);
    det::enq(skey, det::SUBGHZ, "433 burst", nm, (int8_t)subPeak, 'S', false, false, 'M');
  }
  subStrong = 0;
  subPeak = -120;
  subWindowStart = now;
}

// ---------------------------------------------------------------- task

// Power radios down or back up when the focus changes. Task only, under the bus lock.
static Focus focusNow = Focus::All;
static void applyFocus() {
  Focus f = focusReq;
  if (f == focusNow) return;
  bool nrfWas = focusNow == Focus::All || focusNow == Focus::Nrf, ccWas = focusNow == Focus::All || focusNow == Focus::Cc;
  bool nrfOn = f == Focus::All || f == Focus::Nrf, ccOn = f == Focus::All || f == Focus::Cc;
  bus::lock();
  if (nN && nrfWas && !nrfOn) {
    SPI.beginTransaction(NRF_SPI);
    for (int i = 0; i < nN; i++) nrfWrite(nrf[i], 0x00, 0x08);   // PWR_UP = 0
    SPI.endTransaction();
  }
  if (nN && !nrfWas && nrfOn) nrfSetup();
  if (cc && ccWas && !ccOn) {
    SPI.beginTransaction(CC_SPI);
    ccStrobe(0x36); ccStrobe(0x39);   // SIDLE, SPWD
    SPI.endTransaction();
  }
  if (cc && !ccWas && ccOn) { ccSweeping = false; ccInit433(); }
  bus::deselectAll();
  bus::unlock();
  focusNow = f;
  static const char* NAMES[] = {"all", "nrf", "cc", "none"};
  sio::out("[rf] focus %s", NAMES[(int)f]);
}

static void task(void*) {
  if (nN) nrfSetup();
  if (cc) ccInit433();
  for (;;) {
    applyFocus();
    const bool nrfOn = focusNow == Focus::All || focusNow == Focus::Nrf;
    const bool ccOn = focusNow == Focus::All || focusNow == Focus::Cc;
    int segHot[3], segLen[3];
    if (nN && nrfOn) {
      bus::lock();
      nrfSweep(segHot, segLen);
      bus::unlock();
      int h = 0, l = 0;
      for (int i = 0; i < nN; i++) { h += segHot[i]; l += segLen[i]; }
      busy = l ? h * 100 / l : 0;
      judgeJam(segHot, segLen);
      for (int i = 0; i < nN; i++) segP[i] = segLen[i] ? segHot[i] * 100 / segLen[i] : 0;
      portENTER_CRITICAL(&accMux);
      for (int c = 0; c < CH; c++) if (hotNow[c] && acc[c] < 250) acc[c]++;
      if (accN < 60000) accN++;
      portEXIT_CRITICAL(&accMux);
      nSweeps++;
    }
    if (cc && ccOn && ccSweepReq) {
      bus::lock();
      ccSweepOnce();
      bus::unlock();
    } else if (cc && ccOn) {
      if (ccSweeping) { ccSweeping = false; ccInit433(); }   // back to 433.92 watch
      bus::lock();
      int dbm = ccSample();
      bus::unlock();
      judgeSub(dbm);
    }
    if (nrfOn && nSweeps % 250 == 0 && nN) {
      sio::out("[rf] sweeps %lu busy %d%% cc %d (min %d max %d) floor %d", (unsigned long)nSweeps, busy, ccLast,
               subMin, subMax, ccNoise);
      subMin = 0;
      subMax = -200;
    }
    vTaskDelay(pdMS_TO_TICKS(focusNow == Focus::None ? 100 : saver ? 400 : 25));
  }
}

// dev (sdbench): quiet the shield radios in different ways to find what disturbs the SD
// card. Caller holds the bus lock for the whole test. "" or "normal" restores.
void benchQuiet(const char* how) {
  bus::lock();
  bool gdo = !strcmp(how, "gdo"), ccoff = !strcmp(how, "ccoff"), nrfoff = !strcmp(how, "nrfoff"),
       all = !strcmp(how, "alloff");
  if (cc && (gdo || all)) {
    SPI.beginTransaction(CC_SPI);
    ccWrite(0x02, 0x2F); ccWrite(0x00, 0x2F);   // IOCFG2/IOCFG0: static low, no clock out
    SPI.endTransaction();
  }
  if (cc && (ccoff || all)) {
    SPI.beginTransaction(CC_SPI);
    ccStrobe(0x36); ccStrobe(0x39);               // SIDLE, SPWD
    SPI.endTransaction();
  }
  if (nN && (nrfoff || all)) {
    SPI.beginTransaction(NRF_SPI);
    for (int i = 0; i < nN; i++) nrfWrite(nrf[i], 0x00, 0x08);   // PWR_UP = 0
    SPI.endTransaction();
  }
  if (!how[0] || !strcmp(how, "normal")) {
    if (nN) nrfSetup();
    if (cc) { ccSweeping = false; ccInit433(); }
  }
  bus::deselectAll();
  bus::unlock();
}

void begin() {
  static const uint8_t ce[3] = {PIN_NRF1_CE, PIN_NRF2_CE, PIN_NRF3_CE};
  static const uint8_t csn[3] = {PIN_NRF1_CSN, PIN_NRF2_CSN, PIN_NRF3_CSN};
  nN = 0;
  for (int i = 0; i < 3; i++)
    if (post::nrf(i)) { nrf[nN].ce = ce[i]; nrf[nN].csn = csn[i]; nN++; }
  for (int i = 0; i < nN; i++) {
    nrf[i].lo = CH * i / nN;
    nrf[i].hi = CH * (i + 1) / nN;
  }
  cc = post::cc1101();
  sio::out("[rf] nRF24 x%d, CC1101 %s", nN, cc ? "yes" : "no");
  if (!nN && !cc) return;
  bus::deselectAll();
  // Core 0 next to the ESP radio stacks (they run at higher priority and preempt us).
  xTaskCreatePinnedToCore(task, "rfwatch", 4096, nullptr, 1, nullptr, 0);
}

}  // namespace rfwatch
