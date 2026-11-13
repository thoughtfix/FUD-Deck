// FUD-Deck probe: hardware bring-up and investigation firmware for ESP32-DIV V2.
//
// Read and receive only. No radio here ever transmits:
//   nRF24: carrier detect (RPD) in RX only. CC1101: RX and RSSI only.
//   Wi-Fi and BLE: passive scans only. IR: receive edges only.
//
// Talks on both USB ports at once (native USB CDC and the CP2102 UART).
// Type "help" on either.

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <WiFi.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <RF24.h>
#include <esp_chip_info.h>
#include <esp_system.h>
#include <esp_mac.h>
#include <stdarg.h>
#include <vector>
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include "pins.h"

#define PROBE_VERSION "0.2"
// Upright as the board is held (D-pad below the screen). TFT_eSPI's "2" is LovyanGFX's 0 here.
#define PROBE_ROTATION 0

// ------------------------------------------------------------------ display

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ILI9341 _panel;
  lgfx::Bus_SPI _bus;
  lgfx::Light_PWM _light;
  lgfx::Touch_XPT2046 _touch;

 public:
  LGFX() {
    {
      auto c = _bus.config();
      c.spi_host = SPI3_HOST;
      c.spi_mode = 0;
      c.freq_write = 27000000;
      c.freq_read = 16000000;
      c.spi_3wire = false;
      c.use_lock = true;
      c.dma_channel = SPI_DMA_CH_AUTO;
      c.pin_sclk = PIN_TFT_SCLK;
      c.pin_mosi = PIN_TFT_MOSI;
      c.pin_miso = PIN_TFT_MISO;
      c.pin_dc = PIN_TFT_DC;
      _bus.config(c);
      _panel.setBus(&_bus);
    }
    {
      auto c = _panel.config();
      c.pin_cs = PIN_TFT_CS;
      c.pin_rst = PIN_TFT_RST;
      c.pin_busy = -1;
      c.panel_width = 240;
      c.panel_height = 320;
      c.readable = true;
      c.invert = false;
      c.rgb_order = false;
      c.dlen_16bit = false;
      c.bus_shared = true;
      _panel.config(c);
    }
    {
      auto c = _light.config();
      c.pin_bl = PIN_TFT_BL;
      c.invert = false;
      c.freq = 5000;
      c.pwm_channel = 7;
      _light.config(c);
      _panel.setLight(&_light);
    }
    {
      auto c = _touch.config();
      c.x_min = 280;  // stock V2 calibration
      c.x_max = 3850;
      c.y_min = 320;
      c.y_max = 3750;
      c.pin_int = -1;
      c.bus_shared = true;
      c.offset_rotation = 0;
      c.spi_host = SPI3_HOST;
      c.freq = 2500000;
      c.pin_sclk = PIN_TFT_SCLK;
      c.pin_mosi = PIN_TFT_MOSI;
      c.pin_miso = PIN_TFT_MISO;
      c.pin_cs = PIN_TOUCH_CS;
      _touch.config(c);
      _panel.setTouch(&_touch);
    }
    setPanel(&_panel);
  }
  lgfx::Panel_ILI9341& panel() { return _panel; }
};

static LGFX tft;
// WS2812 driver on the core RMT API. Adafruit_NeoPixel shows nothing on this core;
// the core's own rgbLedWrite works, so this does the same thing for all 4 LEDs.
static uint8_t ledBuf[NEOPIXEL_COUNT][3];  // r, g, b
static uint8_t ledBright = 32;
static bool ledInit = false;

static void ledsShow() {
  if (!ledInit) { ledInit = rmtInit(PIN_NEOPIXEL, RMT_TX_MODE, RMT_MEM_NUM_BLOCKS_1, 10000000); }
  rmt_data_t d[NEOPIXEL_COUNT * 24];
  int k = 0;
  for (int i = 0; i < NEOPIXEL_COUNT; i++) {
    uint8_t grb[3] = {ledBuf[i][1], ledBuf[i][0], ledBuf[i][2]};
    for (int c = 0; c < 3; c++) {
      uint8_t v = (uint16_t)grb[c] * ledBright / 255;
      for (int b = 7; b >= 0; b--) {
        bool one = v & (1 << b);
        d[k].level0 = 1; d[k].duration0 = one ? 8 : 4;  // 10 MHz ticks
        d[k].level1 = 0; d[k].duration1 = one ? 4 : 8;
        k++;
      }
    }
  }
  rmtWrite(PIN_NEOPIXEL, d, k, RMT_WAIT_FOR_EVER);
}
static void ledsSet(int i, uint8_t r, uint8_t g, uint8_t b) {
  if (i < 0 || i >= NEOPIXEL_COUNT) return;
  ledBuf[i][0] = r; ledBuf[i][1] = g; ledBuf[i][2] = b;
}
static void ledsFill(uint8_t r, uint8_t g, uint8_t b) { for (int i = 0; i < NEOPIXEL_COUNT; i++) ledsSet(i, r, g, b); }
static void ledsClear() { ledsFill(0, 0, 0); ledsShow(); }

// ------------------------------------------------------------------ output

// Returns false if it gave up. USB CDC drops what doesn't fit, so we retry, but only
// for as long as the caller allows.
static bool writeAll(Print& p, const uint8_t* d, size_t n, uint32_t timeoutMs) {
  uint32_t start = millis();
  while (n) {
    size_t w = p.write(d, n);
    if (w == 0) {
      if (millis() - start >= timeoutMs) return false;
      delay(1);
      continue;
    }
    d += w; n -= w;
  }
  return true;
}

// USB plugged into a computer with no terminal open still reads as connected, but nobody
// drains the buffer. After one stall, stop waiting on USB until a write gets through again.
static bool cdcStalled = false;
static bool reliableOut = false;  // screenshots set this: wait for every byte

static void emitTo(Print& p, const char* s, size_t n, bool isCdc) {
  uint32_t t = reliableOut ? 3000 : (isCdc && cdcStalled ? 0 : 50);
  bool ok = writeAll(p, (const uint8_t*)s, n, t) && writeAll(p, (const uint8_t*)"\n", 1, t);
  if (isCdc) cdcStalled = !ok;
}

static void emit(const char* s) {
  size_t n = strlen(s);
  if (Serial) emitTo(Serial, s, n, true);
  emitTo(Serial0, s, n, false);
}

static void out(const char* fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  emit(buf);
}

// ------------------------------------------------------------------ results

enum Status { ST_PASS, ST_WARN, ST_FAIL, ST_INFO, ST_SKIP };
static const char* ST_NAME[] = {"PASS", "WARN", "FAIL", "INFO", "SKIP"};
static const uint16_t ST_COLOR[] = {TFT_GREEN, TFT_YELLOW, TFT_RED, TFT_CYAN, TFT_DARKGREY};

struct Result { String name; Status st; String detail; };
static std::vector<Result> results;

static void result(const char* name, Status st, const char* fmt, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  out("[res] %-10s %s %s", name, ST_NAME[st], buf);
  for (auto& r : results) {
    if (r.name == name) { r.st = st; r.detail = buf; return; }
  }
  results.push_back({name, st, buf});
}

// ------------------------------------------------------------------ screen helpers

static bool onSummary = false;

static void screenHeader(const char* title) {
  onSummary = false;
  tft.fillScreen(TFT_BLACK);
  tft.fillRect(0, 0, tft.width(), 18, 0x3166);
  tft.setFont(&fonts::Font2);
  tft.setTextColor(TFT_WHITE, 0x3166);
  tft.setTextDatum(textdatum_t::top_left);
  tft.drawString(title, 4, 1);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
}

static void screenPrompt(const char* title, const char* big, const char* small) {
  screenHeader(title);
  tft.setTextDatum(textdatum_t::middle_center);
  tft.setFont(&fonts::Font4);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawString(big, tft.width() / 2, tft.height() / 2 - 20);
  tft.setFont(&fonts::Font2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  if (small) tft.drawString(small, tft.width() / 2, tft.height() / 2 + 20);
  tft.setTextDatum(textdatum_t::top_left);
}

static void screenSummary() {
  screenHeader("FUD PROBE " PROBE_VERSION "  results");
  tft.setFont(&fonts::Font0);
  int y = 24;
  for (auto& r : results) {
    if (y > 204) break;  // below this is the input log
    tft.setTextColor(ST_COLOR[r.st], TFT_BLACK);
    tft.drawString(ST_NAME[r.st], 2, y);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString(r.name, 32, y);
    String d = r.detail;
    if (d.length() > 22) d = d.substring(0, 22);
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.drawString(d, 98, y);
    y += 10;
  }
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString("serial: help", 2, tft.height() - 10);
  onSummary = true;
}

// ------------------------------------------------------------------ serial input

static String lineBuf[2];

static bool pollLine(String& line) {
  Stream* s[2] = {&Serial, &Serial0};
  for (int i = 0; i < 2; i++) {
    while (s[i]->available()) {
      char c = s[i]->read();
      if (c == '\r') continue;
      if (c == '\n') {
        line = lineBuf[i];
        lineBuf[i] = "";
        line.trim();
        if (line.length()) return true;
        continue;
      }
      if (lineBuf[i].length() < 200) lineBuf[i] += c;
    }
  }
  return false;
}

// ------------------------------------------------------------------ I2C

static int pcfAddr = -1;
static bool ip5306Present = false;

static const char* i2cGuess(uint8_t a) {
  if (a >= 0x20 && a <= 0x27) return "PCF8574 (or PN532 at 0x24)";
  if (a >= 0x38 && a <= 0x3F) return "PCF8574A";
  if (a == 0x75) return "IP5306 (I2C variant)";
  if (a == 0x10) return "PA1010D GPS";
  if (a == 0x36) return "MAX17048 fuel gauge";
  if (a == 0x42) return "u-blox GPS";
  if (a == 0x6A || a == 0x6B) return "IMU (LSM6DS3 etc)";
  if (a == 0x68) return "IMU or RTC";
  if (a == 0x48) return "ADC / temp sensor";
  return "unknown";
}

static bool i2cPing(uint8_t a) {
  Wire.beginTransmission(a);
  return Wire.endTransmission() == 0;
}

static int i2cReadReg(uint8_t a, uint8_t reg) {
  Wire.beginTransmission(a);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return -1;
  if (Wire.requestFrom((int)a, 1) != 1) return -1;
  return Wire.read();
}

static void testI2C() {
  out("[i2c] scanning SDA=%d SCL=%d at 100 kHz", PIN_SDA, PIN_SCL);
  int found = 0;
  pcfAddr = -1;
  ip5306Present = false;
  String list;
  for (uint8_t a = 1; a < 0x7F; a++) {
    if (!i2cPing(a)) continue;
    found++;
    out("[i2c]   0x%02X  %s", a, i2cGuess(a));
    char t[8];
    snprintf(t, sizeof(t), "%02X ", a);
    list += t;
    if (a >= 0x20 && a <= 0x27 && pcfAddr < 0) pcfAddr = a;
    if (a == 0x75) ip5306Present = true;
  }
  if (found == 0) result("i2c", ST_FAIL, "no devices. bus dead or pins wrong");
  else result("i2c", ST_PASS, "%d dev: %s", found, list.c_str());
}

static int pcfRead() {
  if (pcfAddr < 0) return -1;
  if (Wire.requestFrom(pcfAddr, 1) != 1) return -1;
  return Wire.read();
}

static void testPCF() {
  if (pcfAddr < 0) { result("pcf8574", ST_FAIL, "not found 0x20-0x27"); return; }
  Wire.beginTransmission(pcfAddr);
  Wire.write(0xFF);  // all quasi-bidirectional pins high = inputs
  Wire.endTransmission();
  int v = pcfRead();
  if (v < 0) { result("pcf8574", ST_FAIL, "0x%02X ack but read failed", pcfAddr); return; }
  // idle with nothing pressed should read all ones on button bits
  uint8_t btnMask = (1 << PCF_BIT_UP) | (1 << PCF_BIT_DOWN) | (1 << PCF_BIT_LEFT) | (1 << PCF_BIT_RIGHT) | (1 << PCF_BIT_SELECT);
  if ((v & btnMask) != btnMask)
    result("pcf8574", ST_WARN, "0x%02X idle=0x%02X button bit low", pcfAddr, v);
  else
    result("pcf8574", ST_PASS, "0x%02X idle=0x%02X", pcfAddr, v);
}

// IP5306 I2C variant. Reads only. Register meanings from the IP5306_I2C datasheet.
static void testIP5306() {
  if (!ip5306Present) {
    result("battery", ST_WARN, "no IP5306 at 0x75. plain IP5306 has no I2C");
    return;
  }
  int r70 = i2cReadReg(0x75, 0x70);
  int r71 = i2cReadReg(0x75, 0x71);
  int r72 = i2cReadReg(0x75, 0x72);
  int r77 = i2cReadReg(0x75, 0x77);
  int r78 = i2cReadReg(0x75, 0x78);
  out("[bat] IP5306 regs 70=%02X 71=%02X 72=%02X 77=%02X 78=%02X", r70 & 0xFF, r71 & 0xFF, r72 & 0xFF, r77 & 0xFF, r78 & 0xFF);
  for (uint8_t r = 0x00; r <= 0x03; r++) out("[bat]   ctl %02X=%02X", r, i2cReadReg(0x75, r) & 0xFF);
  if (r78 < 0) { result("battery", ST_FAIL, "IP5306 read failed"); return; }
  int level;
  switch (r78 & 0xF0) {
    case 0x00: level = 100; break;
    case 0x80: level = 75; break;
    case 0xC0: level = 50; break;
    case 0xE0: level = 25; break;
    default: level = 0; break;
  }
  bool vin = r70 >= 0 && (r70 & 0x08);
  bool full = r71 >= 0 && (r71 & 0x08);
  result("battery", ST_PASS, "~%d%% vin=%d full=%d", level, vin, full);
}

// ------------------------------------------------------------------ battery ADC (IO2)

// Schematic: VBAT > R11 100k > IO2 > R16 100k > GND, so IO2 = VBAT/2.
// IO2 also drives the speaker transistor Q1 through R5 1k, which loads the divider.
static void testVbatAdc() {
  noTone(PIN_BUZZER);
  pinMode(PIN_BUZZER, INPUT);
  analogSetPinAttenuation(PIN_BUZZER, ADC_11db);
  delay(5);
  uint32_t sum = 0;
  for (int i = 0; i < 32; i++) { sum += analogReadMilliVolts(PIN_BUZZER); delayMicroseconds(300); }
  float mv = sum / 32.0f;
  // Our unit: R11 = 4.7k, R16 = 10k (not 100k/100k as drawn), so VBAT = IO2 x 14.7/10.
  float v = mv * VBAT_RATIO / 1000.0f;
  out("[vbat] IO2 reads %.0f mV > VBAT %.2f V (x%.2f)", mv, v, VBAT_RATIO);
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);
  Status st = (v > 3.0f && v < 4.45f) ? ST_PASS : ST_WARN;
  result("vbat-adc", st, "%.2fV (IO2 %.0fmV)%s", v, mv, st == ST_WARN ? " implausible: Q1 clamp?" : "");
}

// ------------------------------------------------------------------ I2C speed

static void testI2CSpeed() {
  const uint32_t speeds[] = {100000, 400000, 1000000};
  String sum;
  for (uint32_t hz : speeds) {
    Wire.setClock(hz);
    int err = 0;
    uint32_t t0 = micros();
    for (int i = 0; i < 500; i++) {
      if (pcfAddr >= 0 && pcfRead() < 0) err++;
      if (ip5306Present && i2cReadReg(0x75, 0x70) < 0) err++;
    }
    uint32_t us = micros() - t0;
    out("[i2cspeed] %4lu kHz: %d errors in 500 rounds, %lu us/round", (unsigned long)(hz / 1000), err, (unsigned long)(us / 500));
    char t[24];
    snprintf(t, sizeof(t), "%luk:%d ", (unsigned long)(hz / 1000), err);
    sum += t;
  }
  Wire.setClock(100000);
  result("i2cspeed", ST_INFO, "errors %s", sum.c_str());
}

// ------------------------------------------------------------------ I2C line finder

// Holds SCL low, then SDA low, so a meter on the umbilical can tell which hole is which.
// Only the I2C bus is touched, and only by pulling it low (what any I2C master does).
static void i2cIdentify(int secs) {
  const struct { const char* n; int pin; } line[2] = {{"SCL", PIN_SCL}, {"SDA", PIN_SDA}};
  Wire.end();
  for (auto& l : line) {
    pinMode(PIN_SCL, INPUT_PULLUP);
    pinMode(PIN_SDA, INPUT_PULLUP);
    pinMode(l.pin, OUTPUT);
    digitalWrite(l.pin, LOW);
    char big[24];
    snprintf(big, sizeof(big), "%s held LOW", l.n);
    screenPrompt("I2C LINE FINDER", big, "the other I2C line stays ~3.3 V");
    out("[i2cid] %s (GPIO%d) LOW for %d s. Find the hole reading 0 V.", l.n, l.pin, secs);
    uint32_t t0 = millis();
    String in;
    while (millis() - t0 < (uint32_t)secs * 1000) {
      if (pollLine(in) && in == "q") { secs = 0; break; }
      delay(20);
    }
  }
  pinMode(PIN_SCL, INPUT_PULLUP);
  pinMode(PIN_SDA, INPUT_PULLUP);
  Wire.begin(PIN_SDA, PIN_SCL, 100000);
  testI2C();  // prove the bus came back
  screenSummary();
}

// ------------------------------------------------------------------ free pins

// Pull test: a floating pin follows the internal pull. A connected pin does not.
static const char* pullTest(uint8_t p, int* hi, int* lo) {
  pinMode(p, INPUT_PULLUP);
  delay(3);
  *hi = digitalRead(p);
  pinMode(p, INPUT_PULLDOWN);
  delay(3);
  *lo = digitalRead(p);
  pinMode(p, INPUT_PULLUP);
  if (*hi == 1 && *lo == 0) return "floating (no strong driver)";
  if (*hi == 1 && *lo == 1) return "held HIGH (connected)";
  if (*hi == 0 && *lo == 0) return "held LOW (connected)";
  return "odd (follows opposite)";
}

static void testFreePins() {
  String sum;
  for (uint8_t p : FREE_PINS) {
    int hi, lo;
    const char* v = pullTest(p, &hi, &lo);
    out("[pins] GPIO%-2d pullup=%d pulldown=%d  %s", p, hi, lo, v);
    char t[16];
    snprintf(t, sizeof(t), "%d:%c ", p, (hi == 1 && lo == 0) ? 'f' : (hi ? 'H' : 'L'));
    sum += t;
  }
  result("freepins", ST_INFO, "%s", sum.c_str());
}

static uint8_t freeSnapshot() {
  uint8_t m = 0;
  for (size_t i = 0; i < sizeof(FREE_PINS); i++)
    if (digitalRead(FREE_PINS[i])) m |= (1 << i);
  return m;
}

static uint8_t freeChanged = 0;  // pins seen changing during the last watched action
static void freeWatchStart() { for (uint8_t p : FREE_PINS) pinMode(p, INPUT_PULLUP); freeChanged = 0; }
static void freeWatchTick(uint8_t base) { freeChanged |= (freeSnapshot() ^ base); }
static String freeChangedList() {
  String s;
  for (size_t i = 0; i < sizeof(FREE_PINS); i++)
    if (freeChanged & (1 << i)) { s += "GPIO"; s += FREE_PINS[i]; s += " "; }
  return s;
}

// ------------------------------------------------------------------ SPI bus

static void spiDeselectAll() {
  const uint8_t cs[] = {PIN_SD_CS, PIN_NRF1_CSN, PIN_NRF2_CSN, PIN_NRF3_CSN, PIN_CC_CS};
  for (uint8_t p : cs) { pinMode(p, OUTPUT); digitalWrite(p, HIGH); }
  const uint8_t ce[] = {PIN_NRF1_CE, PIN_NRF2_CE, PIN_NRF3_CE};
  for (uint8_t p : ce) { pinMode(p, OUTPUT); digitalWrite(p, LOW); }
}

// Some SD cards keep driving MISO after CS goes high, until they see clocks with CS high.
// On this board that silences every radio on the shared bus. Clock them loose.
static void sdRelease() {
  spiDeselectAll();
  // A failed SD.begin can leave the bus unusable, so start it fresh first.
  SPI.end();
  SPI.begin(PIN_SPI_SCK, PIN_SPI_MISO, PIN_SPI_MOSI, -1);
  SPI.beginTransaction(SPISettings(400000, MSBFIRST, SPI_MODE0));
  for (int i = 0; i < 10; i++) SPI.transfer(0xFF);
  SPI.endTransaction();
}

// ------------------------------------------------------------------ raw bus diagnosis

// Bit-banged, slow, no SPI peripheral. For each chip select: is anyone driving MISO,
// and what byte comes back for a harmless command (nRF24 NOP 0xFF returns STATUS).
static uint8_t bbXfer(uint8_t v) {
  uint8_t r = 0;
  for (int i = 7; i >= 0; i--) {
    digitalWrite(PIN_SPI_MOSI, (v >> i) & 1);
    delayMicroseconds(20);
    digitalWrite(PIN_SPI_SCK, HIGH);
    delayMicroseconds(20);
    r = (r << 1) | digitalRead(PIN_SPI_MISO);
    digitalWrite(PIN_SPI_SCK, LOW);
  }
  return r;
}

static int misoDriven(int* hi, int* lo) {
  pinMode(PIN_SPI_MISO, INPUT_PULLUP); delayMicroseconds(200); *hi = digitalRead(PIN_SPI_MISO);
  pinMode(PIN_SPI_MISO, INPUT_PULLDOWN); delayMicroseconds(200); *lo = digitalRead(PIN_SPI_MISO);
  pinMode(PIN_SPI_MISO, INPUT);
  return !(*hi == 1 && *lo == 0);
}

static void spiDiag() {
  SPI.end();
  spiDeselectAll();
  pinMode(PIN_SPI_SCK, OUTPUT); digitalWrite(PIN_SPI_SCK, LOW);
  pinMode(PIN_SPI_MOSI, OUTPUT); digitalWrite(PIN_SPI_MOSI, LOW);
  int hi, lo;
  int idle = misoDriven(&hi, &lo);
  out("[spidiag] no CS: MISO pullup=%d pulldown=%d  %s", hi, lo, idle ? "DRIVEN (something stuck on bus)" : "floating (good)");
  struct { const char* n; uint8_t cs; bool nrf; } dev[] = {
    {"sd", PIN_SD_CS, false}, {"nrf1", PIN_NRF1_CSN, true}, {"nrf2", PIN_NRF2_CSN, true},
    {"nrf3", PIN_NRF3_CSN, true}, {"cc1101", PIN_CC_CS, false},
  };
  String sum;
  for (auto& d : dev) {
    spiDeselectAll();
    digitalWrite(d.cs, LOW);
    delayMicroseconds(500);
    int drv = misoDriven(&hi, &lo);
    int st = -1;
    if (d.nrf) {
      st = bbXfer(0xFF);  // NOP: chip clocks out STATUS
    }
    digitalWrite(d.cs, HIGH);
    out("[spidiag] %-6s CS=GPIO%-2d MISO pullup=%d pulldown=%d %s%s", d.n, d.cs, hi, lo,
        drv ? "driven" : "FLOATING (no chip answering)",
        st >= 0 ? (String("  STATUS=0x") + String(st, HEX) + (st == 0x0E ? " (healthy)" : "")).c_str() : "");
    sum += d.n; sum += drv ? "+ " : "- ";
  }
  spiDeselectAll();
  SPI.begin(PIN_SPI_SCK, PIN_SPI_MISO, PIN_SPI_MOSI, -1);
  result("spidiag", ST_INFO, "%s", sum.c_str());
}

// ------------------------------------------------------------------ SD

static void testSD() {
  pinMode(PIN_SD_CD, INPUT_PULLUP);
  int cd = digitalRead(PIN_SD_CD);
  spiDeselectAll();
  bool ok = SD.begin(PIN_SD_CS, SPI, 4000000);
  if (!ok) {
    sdRelease();
    result("sd", ST_WARN, "no card mounted (CD pin=%d)", cd);
    return;
  }
  uint8_t t = SD.cardType();
  const char* tn = t == CARD_MMC ? "MMC" : t == CARD_SD ? "SD" : t == CARD_SDHC ? "SDHC" : "?";
  uint64_t mb = SD.cardSize() / (1024 * 1024);
  bool rw = false;
  File f = SD.open("/fudprobe.tmp", FILE_WRITE);
  if (f) { f.print("fud"); f.close(); }
  f = SD.open("/fudprobe.tmp");
  if (f) { rw = f.readString() == "fud"; f.close(); }
  SD.remove("/fudprobe.tmp");
  int n = 0;
  File root = SD.open("/");
  if (root) {
    File e;
    while ((e = root.openNextFile())) { if (n < 10) out("[sd]   %s%s", e.name(), e.isDirectory() ? "/" : ""); n++; e.close(); }
    root.close();
  }
  SD.end();
  sdRelease();
  result("sd", rw ? ST_PASS : ST_WARN, "%s %lluMB rw=%d CD=%d files=%d", tn, mb, rw, cd, n);
}

// ------------------------------------------------------------------ nRF24 (RX only)

struct NrfDef { const char* name; uint8_t ce, csn; };
static const NrfDef NRF[3] = {
  {"nrf1", PIN_NRF1_CE, PIN_NRF1_CSN},
  {"nrf2", PIN_NRF2_CE, PIN_NRF2_CSN},
  {"nrf3", PIN_NRF3_CE, PIN_NRF3_CSN},
};

// Sweep 0-125 with carrier detect. Fills hits[126]. Receive only.
static bool nrfSweep(int i, int passes, uint16_t* hits, bool* plus) {
  spiDeselectAll();
  RF24 r(NRF[i].ce, NRF[i].csn);
  if (!r.begin(&SPI) || !r.isChipConnected()) { spiDeselectAll(); return false; }
  if (plus) *plus = r.isPVariant();
  r.setAutoAck(false);
  r.setDataRate(RF24_2MBPS);
  r.startListening();
  r.stopListening();
  memset(hits, 0, 126 * sizeof(uint16_t));
  for (int p = 0; p < passes; p++) {
    for (int ch = 0; ch < 126; ch++) {
      r.setChannel(ch);
      r.startListening();
      delayMicroseconds(170);
      r.stopListening();
      if (r.testRPD()) hits[ch]++;
    }
  }
  r.powerDown();
  spiDeselectAll();
  return true;
}

static void testNrf(int i, int passes) {
  uint16_t hits[126];
  bool plus = false;
  if (!nrfSweep(i, passes, hits, &plus)) {
    result(NRF[i].name, ST_FAIL, "no response CE=%d CSN=%d", NRF[i].ce, NRF[i].csn);
    return;
  }
  int busiest = 0;
  uint32_t total = 0;
  for (int ch = 0; ch < 126; ch++) { total += hits[ch]; if (hits[ch] > hits[busiest]) busiest = ch; }
  result(NRF[i].name, ST_PASS, "%s hits=%lu top ch%d", plus ? "nRF24L01+" : "nRF24L01", (unsigned long)total, busiest);
}

static void nrfScanPrint(int i, int passes) {
  uint16_t hits[126];
  if (!nrfSweep(i, passes, hits, nullptr)) { out("[nrf] %s not responding", NRF[i].name); return; }
  const char* g = " .:-=+*#%@";
  String row;
  for (int ch = 0; ch < 126; ch++) {
    int v = hits[ch] * 9 / (passes ? passes : 1);
    row += g[v > 9 ? 9 : v];
  }
  out("[nrf] %s  ch0 (2400 MHz) ... ch125 (2525 MHz), %d passes", NRF[i].name, passes);
  out("[nrf] |%s|", row.c_str());
  out("[nrf]  wifi 1 ~ch12, 6 ~ch37, 11 ~ch62");
}

// CE readback: drive each nRF24 CE high for a moment and read the pad back.
// A pin that reads 0 while driven high is being clamped by something (IO14: IR LED transistor).
static void ceTest() {
  String sum;
  for (int i = 0; i < 3; i++) {
    gpio_num_t g = (gpio_num_t)NRF[i].ce;
    gpio_reset_pin(g);
    gpio_set_direction(g, GPIO_MODE_INPUT_OUTPUT);
    gpio_set_level(g, 1);
    delayMicroseconds(200);
    int hi = gpio_get_level(g);
    gpio_set_level(g, 0);
    delayMicroseconds(200);
    int lo = gpio_get_level(g);
    pinMode(NRF[i].ce, OUTPUT);
    digitalWrite(NRF[i].ce, LOW);
    out("[cetest] %s CE GPIO%d: driven 1 reads %d, driven 0 reads %d%s", NRF[i].name, NRF[i].ce, hi, lo,
        hi ? "" : "  CLAMPED (cannot go high)");
    sum += NRF[i].name; sum += hi ? ":ok " : ":CLAMPED ";
  }
  result("nrf-ce", ST_INFO, "%s", sum.c_str());
}

// ------------------------------------------------------------------ CC1101 (RX only)

static SPISettings ccSpi(4000000, MSBFIRST, SPI_MODE0);

static bool ccSelect() {
  digitalWrite(PIN_CC_CS, LOW);
  uint32_t t = micros();
  while (digitalRead(PIN_SPI_MISO)) {  // chip ready when SO goes low
    if (micros() - t > 3000) return false;
  }
  return true;
}

static uint8_t ccXfer(uint8_t a, uint8_t v) {
  SPI.beginTransaction(ccSpi);
  ccSelect();
  SPI.transfer(a);
  uint8_t r = SPI.transfer(v);
  digitalWrite(PIN_CC_CS, HIGH);
  SPI.endTransaction();
  return r;
}

static uint8_t ccStrobe(uint8_t s) {
  SPI.beginTransaction(ccSpi);
  ccSelect();
  uint8_t r = SPI.transfer(s);
  digitalWrite(PIN_CC_CS, HIGH);
  SPI.endTransaction();
  return r;
}

static uint8_t ccStatus(uint8_t a) { return ccXfer(a | 0xC0, 0); }
static uint8_t ccReadReg(uint8_t a) { return ccXfer(a | 0x80, 0); }
static void ccWriteReg(uint8_t a, uint8_t v) { ccXfer(a, v); }

static void ccSetFreq(uint32_t hz) {
  uint32_t w = (uint32_t)(((uint64_t)hz << 16) / 26000000ULL);
  ccWriteReg(0x0D, (w >> 16) & 0xFF);
  ccWriteReg(0x0E, (w >> 8) & 0xFF);
  ccWriteReg(0x0F, w & 0xFF);
}

static float ccRssiAt(uint32_t hz, uint8_t* marc) {
  ccStrobe(0x36);  // SIDLE
  ccSetFreq(hz);
  ccStrobe(0x33);  // SCAL
  delay(2);
  ccStrobe(0x34);  // SRX
  delay(3);
  if (marc) *marc = ccStatus(0x35) & 0x1F;
  uint8_t raw = ccStatus(0x34);
  ccStrobe(0x36);  // SIDLE
  int v = raw >= 128 ? (int)raw - 256 : raw;
  return v / 2.0f - 74.0f;
}

static void testCC1101() {
  spiDeselectAll();
  pinMode(PIN_CC_GDO0, INPUT);
  pinMode(PIN_CC_GDO2, INPUT);
  if (!(SPI.beginTransaction(ccSpi), ccSelect())) {
    digitalWrite(PIN_CC_CS, HIGH);
    SPI.endTransaction();
    result("cc1101", ST_FAIL, "SO never went low (no chip / CS %d wrong)", PIN_CC_CS);
    return;
  }
  digitalWrite(PIN_CC_CS, HIGH);
  SPI.endTransaction();
  ccStrobe(0x30);  // SRES
  delay(5);
  uint8_t part = ccStatus(0x30), ver = ccStatus(0x31);
  out("[cc] PARTNUM=0x%02X VERSION=0x%02X (expect 00 / 14 or 04)", part, ver);
  if (ver == 0x00 || ver == 0xFF) { result("cc1101", ST_FAIL, "bad VERSION 0x%02X", ver); return; }

  // GDO wiring: force each GDO low then high and read it back on the ESP side.
  ccWriteReg(0x00, 0x2F); ccWriteReg(0x02, 0x2F);  // IOCFG2/0 = HW to 0
  delay(1);
  int g2l = digitalRead(PIN_CC_GDO2), g0l = digitalRead(PIN_CC_GDO0);
  ccWriteReg(0x00, 0x6F); ccWriteReg(0x02, 0x6F);  // inverted = 1
  delay(1);
  int g2h = digitalRead(PIN_CC_GDO2), g0h = digitalRead(PIN_CC_GDO0);
  ccWriteReg(0x00, 0x29); ccWriteReg(0x02, 0x3F);  // back to reset defaults
  bool gdo0 = g0l == 0 && g0h == 1, gdo2 = g2l == 0 && g2h == 1;
  out("[cc] GDO0 (GPIO%d) %d>%d %s, GDO2 (GPIO%d) %d>%d %s", PIN_CC_GDO0, g0l, g0h, gdo0 ? "ok" : "NOT WIRED",
      PIN_CC_GDO2, g2l, g2h, gdo2 ? "ok" : "NOT WIRED");

  const uint32_t f[] = {315000000, 433920000, 868350000, 915000000};
  String rs;
  bool rxOk = true;
  for (uint32_t hz : f) {
    uint8_t marc = 0;
    float dbm = ccRssiAt(hz, &marc);
    out("[cc] RX %.2f MHz  RSSI %.1f dBm  MARCSTATE=0x%02X%s", hz / 1e6, dbm, marc, marc == 0x0D ? " (RX)" : "");
    if (marc != 0x0D) rxOk = false;
    char t[24];
    snprintf(t, sizeof(t), "%d:%.0f ", (int)(hz / 1000000), dbm);
    rs += t;
  }
  Status st = (gdo0 && gdo2 && rxOk) ? ST_PASS : ST_WARN;
  result("cc1101", st, "v%02X gdo0=%d gdo2=%d rx=%d", ver, gdo0, gdo2, rxOk);
  result("cc-rssi", ST_INFO, "%s", rs.c_str());
}

// ------------------------------------------------------------------ Wi-Fi / BLE (passive)

static void testWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  int n = WiFi.scanNetworks(false, true, true, 200);  // passive: we send nothing
  if (n < 0) { result("wifi", ST_FAIL, "scan error %d", n); WiFi.mode(WIFI_OFF); return; }
  int best = -127;
  for (int i = 0; i < n; i++) if (WiFi.RSSI(i) > best) best = WiFi.RSSI(i);
  result("wifi", n > 0 ? ST_PASS : ST_WARN, "%d APs (passive) best %ddBm", n, n ? best : 0);
  WiFi.scanDelete();
  WiFi.mode(WIFI_OFF);
}

static void testBLE() {
  BLEDevice::init("");
  BLEScan* s = BLEDevice::getScan();
  s->setActiveScan(false);  // passive: we send no scan requests
  s->setInterval(100);
  s->setWindow(99);
  BLEScanResults* r = s->start(4, false);
  int n = r ? r->getCount() : 0;
  s->clearResults();
  BLEDevice::deinit(false);
  result("ble", n > 0 ? ST_PASS : ST_WARN, "%d advertisers in 4s (passive)", n);
}

// ------------------------------------------------------------------ info

static void testInfo() {
  esp_chip_info_t ci;
  esp_chip_info(&ci);
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  bool embPsram = ci.features & CHIP_FEATURE_EMB_PSRAM;
  out("[info] %s rev %d.%d, %d cores, %lu MHz", ESP.getChipModel(), ci.revision / 100, ci.revision % 100, ci.cores,
      (unsigned long)ESP.getCpuFreqMHz());
  out("[info] flash %lu MB, PSRAM enabled %lu bytes, embedded PSRAM in chip: %s", (unsigned long)(ESP.getFlashChipSize() >> 20),
      (unsigned long)ESP.getPsramSize(), embPsram ? "yes" : "no");
  out("[info] heap free %lu, sketch %lu, reset reason %d", (unsigned long)ESP.getFreeHeap(),
      (unsigned long)ESP.getSketchSize(), (int)esp_reset_reason());
  out("[info] MAC %02X:%02X:%02X:%02X:%02X:%02X  temp %.1f C", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], temperatureRead());
  result("chip", ST_PASS, "%luMB flash", (unsigned long)(ESP.getFlashChipSize() >> 20));
  // esptool reports 8 MB embedded PSRAM even when this feature flag reads 0.
  result("psram", ST_INFO, "off on purpose: octal PSRAM uses GPIO33-37, TFT is on 35-37");
}

// ------------------------------------------------------------------ guided tests

static int btnUp = -1, btnDown = -1, btnLeft = -1, btnRight = -1, btnSelect = -1;
static bool guidedAbort = false;

// Wait for a PCF8574 bit to go low and come back. Returns bit or -1. Also watches free pins.
static int waitButton(uint32_t timeoutMs, int* rawPressed) {
  int idle = pcfRead();
  if (idle < 0) return -1;
  freeWatchStart();
  uint8_t base = freeSnapshot();
  uint32_t t0 = millis();
  String line;
  while (millis() - t0 < timeoutMs) {
    if (pollLine(line)) {
      if (line == "q") { guidedAbort = true; return -1; }
      if (line == "s") return -1;
    }
    freeWatchTick(base);
    int v = pcfRead();
    if (v >= 0 && v != idle) {
      uint8_t down = idle & ~v;  // bits that went low
      if (rawPressed) *rawPressed = v;
      int bit = -1;
      for (int b = 0; b < 8; b++) if (down & (1 << b)) { bit = b; break; }
      uint32_t t1 = millis();
      while (pcfRead() != idle && millis() - t1 < 5000) { freeWatchTick(base); delay(5); }
      delay(40);
      return bit;
    }
    delay(5);
  }
  return -1;
}

// Yes/No: serial y/n, or the learned UP (yes) / DOWN (no) buttons.
static int ask(const char* title, const char* q, const char* q2 = nullptr) {
  out("[ask] %s  (answer: y/n on serial, or UP=yes DOWN=no)", q);
  tft.fillRect(0, tft.height() - 44, tft.width(), 44, TFT_BLACK);
  tft.setFont(&fonts::Font2);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.setTextDatum(textdatum_t::top_center);
  tft.drawString(q, tft.width() / 2, tft.height() - 42);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(q2 ? q2 : "UP = yes   DOWN = no", tft.width() / 2, tft.height() - 22);
  tft.setTextDatum(textdatum_t::top_left);
  int idle = pcfRead();
  uint32_t t0 = millis();
  String line;
  while (millis() - t0 < 60000) {
    if (pollLine(line)) {
      if (line == "y") return 1;
      if (line == "n") return 0;
      if (line == "q") { guidedAbort = true; return -1; }
    }
    int v = pcfRead();
    if (v >= 0 && idle >= 0 && v != idle) {
      int ans = -1;
      if (btnUp >= 0 && !(v & (1 << btnUp))) ans = 1;
      if (btnDown >= 0 && !(v & (1 << btnDown))) ans = 0;
      uint32_t t1 = millis();
      while (pcfRead() != idle && millis() - t1 < 5000) delay(5);
      delay(40);
      if (ans >= 0) return ans;
    }
    delay(10);
  }
  return -1;
}

static void guidedButtons() {
  struct { const char* name; int* store; int stockBit; } steps[] = {
    {"UP", &btnUp, PCF_BIT_UP},       {"DOWN", &btnDown, PCF_BIT_DOWN},
    {"LEFT", &btnLeft, PCF_BIT_LEFT}, {"RIGHT", &btnRight, PCF_BIT_RIGHT},
    {"CENTER", &btnSelect, PCF_BIT_SELECT},
  };
  if (pcfAddr < 0) { result("buttons", ST_SKIP, "no PCF8574"); return; }
  String map;
  int matched = 0, got = 0;
  uint8_t pinsSeen = 0;
  for (auto& s : steps) {
    if (guidedAbort) return;
    char big[32];
    snprintf(big, sizeof(big), "Press %s", s.name);
    screenPrompt("BUTTONS", big, "the D-pad button in that position");
    out("[do] Press the %s button of the D-pad (20 s, 's' skips)", s.name);
    int raw = -1;
    int bit = waitButton(20000, &raw);
    pinsSeen |= freeChanged;
    *s.store = bit;
    if (bit < 0) { out("[btn] %s: nothing", s.name); map += s.name; map += "=? "; continue; }
    got++;
    if (bit == s.stockBit) matched++;
    out("[btn] %s = P%d (raw 0x%02X)%s", s.name, bit, raw, bit == s.stockBit ? "" : "  DIFFERS FROM STOCK MAP");
    char t[16];
    snprintf(t, sizeof(t), "%c=P%d ", s.name[0], bit);
    map += t;
  }
  result("buttons", got == 5 ? (matched == 5 ? ST_PASS : ST_WARN) : ST_FAIL, "%s(%d/5 stock)", map.c_str(), matched);
  freeChanged = pinsSeen;
  String fc = freeChangedList();
  if (fc.length()) result("pcf-int", ST_INFO, "changed on press: %s", fc.c_str());
  else result("pcf-int", ST_INFO, "no free GPIO toggled. INT not routed to 39-46");

  if (guidedAbort) return;
  screenPrompt("BUTTONS", "Any other button?", "press it now, or wait 8 s");
  out("[do] If there is any other button besides RESET and BOOT, press it now (8 s)");
  int raw = -1;
  int bit = waitButton(8000, &raw);
  if (bit >= 0) result("btn-extra", ST_INFO, "extra button on P%d", bit);
}

static void guidedLCD() {
  int w = tft.width(), h = tft.height();
  screenHeader("LCD COLORS");
  int bh = (h - 18 - 48) / 4;
  uint16_t c[4] = {TFT_RED, TFT_GREEN, TFT_BLUE, TFT_WHITE};
  const char* n[4] = {"RED", "GREEN", "BLUE", "WHITE"};
  tft.setFont(&fonts::Font2);
  for (int i = 0; i < 4; i++) {
    tft.fillRect(0, 18 + i * bh, w, bh, c[i]);
    tft.setTextColor(TFT_BLACK, c[i]);
    tft.drawString(n[i], 4, 22 + i * bh);
  }
  out("[do] Look at the screen: four bars, top to bottom, labeled RED GREEN BLUE WHITE");
  int a = ask("LCD", "Bar colors match labels?");
  if (a == 1) { result("lcd", ST_PASS, "colors correct, rotation %d", tft.getRotation()); return; }
  if (a < 0) { result("lcd", ST_SKIP, "no answer"); return; }
  int sw = ask("LCD", "RED and BLUE swapped?");
  int inv = sw == 1 ? 0 : ask("LCD", "Colors inverted? (white bar dark)");
  result("lcd", ST_WARN, "wrong colors: rgb_swap=%d invert=%d", sw, inv);
}

static void guidedBacklight() {
  screenPrompt("BACKLIGHT", "Watch brightness", "it will dim, then come back");
  out("[do] Watch the screen brightness");
  delay(800);
  for (int b = 255; b >= 8; b -= 4) { tft.setBrightness(b); delay(12); }
  delay(400);
  for (int b = 8; b <= 255; b += 4) { tft.setBrightness(b); delay(12); }
  tft.setBrightness(255);
  int a = ask("BACKLIGHT", "Did it dim and brighten?");
  result("backlight", a == 1 ? ST_PASS : a == 0 ? ST_FAIL : ST_SKIP, "PWM on GPIO%d", PIN_TFT_BL);
}

static void guidedTouch() {
  struct P { int x, y; const char* n; } pts[] = {
    {20, 40, "top-left"}, {220, 40, "top-right"}, {120, 180, "center"}, {20, 300, "bottom-left"}, {220, 300, "bottom-right"},
  };
  int rx[5], ry[5], got = 0;
  freeWatchStart();
  uint8_t base = freeSnapshot();
  uint8_t pinsSeen = 0;
  for (int i = 0; i < 5 && !guidedAbort; i++) {
    screenHeader("TOUCH");
    tft.setFont(&fonts::Font2);
    tft.setTextDatum(textdatum_t::middle_center);
    tft.drawString("Tap the cross with a fingernail", 120, 120);
    tft.setTextDatum(textdatum_t::top_left);
    tft.drawLine(pts[i].x - 12, pts[i].y, pts[i].x + 12, pts[i].y, TFT_YELLOW);
    tft.drawLine(pts[i].x, pts[i].y - 12, pts[i].x, pts[i].y + 12, TFT_YELLOW);
    tft.drawCircle(pts[i].x, pts[i].y, 6, TFT_YELLOW);
    out("[do] Tap the yellow cross at the %s (20 s, 's' skips)", pts[i].n);
    rx[i] = ry[i] = -1;
    uint32_t t0 = millis();
    String line;
    while (millis() - t0 < 20000) {
      if (pollLine(line)) { if (line == "q") { guidedAbort = true; break; } if (line == "s") break; }
      freeWatchTick(base);
      lgfx::touch_point_t tp;
      if (tft.getTouchRaw(&tp, 1)) {
        long sx = 0, sy = 0; int n = 0;
        uint32_t t1 = millis();
        while (millis() - t1 < 300) {
          if (tft.getTouchRaw(&tp, 1)) { sx += tp.x; sy += tp.y; n++; }
          freeWatchTick(base);
          delay(10);
        }
        if (n) { rx[i] = sx / n; ry[i] = sy / n; }
        while (tft.getTouchRaw(&tp, 1)) { freeWatchTick(base); delay(10); }
        break;
      }
      delay(10);
    }
    pinsSeen |= freeChanged;
    if (rx[i] >= 0) { got++; out("[touch] %s screen(%d,%d) raw(%d,%d)", pts[i].n, pts[i].x, pts[i].y, rx[i], ry[i]); }
    else out("[touch] %s: nothing", pts[i].n);
    delay(300);
  }
  if (got == 0) { result("touch", ST_FAIL, "no touches read (CS %d)", PIN_TOUCH_CS); return; }
  // Raw extremes give a calibration hint. Note any axis swap/inversion from corner order.
  int minx = 99999, maxx = -1, miny = 99999, maxy = -1;
  for (int i = 0; i < 5; i++) if (rx[i] >= 0) {
    minx = min(minx, rx[i]); maxx = max(maxx, rx[i]); miny = min(miny, ry[i]); maxy = max(maxy, ry[i]);
  }
  result("touch", got == 5 ? ST_PASS : ST_WARN, "%d/5 raw x%d-%d y%d-%d", got, minx, maxx, miny, maxy);
  freeChanged = pinsSeen;
  String fc = freeChangedList();
  result("touch-irq", ST_INFO, "%s", fc.length() ? ("changed on touch: " + fc).c_str() : "no free GPIO toggled");
}

static void guidedLEDs() {
  screenPrompt("LEDS", "Look at the LEDs", "4 NeoPixels: red green blue white");
  out("[do] Look at the board LEDs. Expect 4 lit: red, green, blue, white");
  ledsSet(0, 255, 0, 0); ledsSet(1, 0, 255, 0); ledsSet(2, 0, 0, 255); ledsSet(3, 255, 255, 255);
  ledsShow();
  int a = ask("LEDS", "4 lit: red green blue white?");
  int any = a == 0 ? ask("LEDS", "Are ANY of them lit?") : 1;
  ledsClear();
  if (a == 1) result("leds", ST_PASS, "4x WS2812 on GPIO%d", PIN_NEOPIXEL);
  else if (a < 0) result("leds", ST_SKIP, "no answer");
  else result("leds", any == 1 ? ST_WARN : ST_FAIL, "%s", any == 1 ? "some lit, order/colors off" : "none lit on GPIO1");
}

static void guidedBuzzer() {
  screenPrompt("SPEAKER", "Listen", "three rising beeps");
  out("[do] Listen for three rising beeps");
  delay(500);
  const int f[3] = {1000, 2000, 3000};
  for (int hz : f) { tone(PIN_BUZZER, hz, 150); delay(300); }
  noTone(PIN_BUZZER);
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);
  int a = ask("SPEAKER", "Heard three beeps?");
  result("speaker", a == 1 ? ST_PASS : a == 0 ? ST_FAIL : ST_SKIP, "GPIO%d via Q1", PIN_BUZZER);
}

static volatile uint32_t irEdges = 0;
static void IRAM_ATTR irIsr() { irEdges++; }

static void guidedIR() {
  spiDeselectAll();
  pinMode(PIN_IR_RX, INPUT_PULLUP);
  int idle = digitalRead(PIN_IR_RX);
  screenPrompt("IR RECEIVE", "Point a remote here", "press any buttons for 10 s");
  out("[do] Point any IR remote (TV etc) at the board and press buttons for 10 s. 's' skips");
  irEdges = 0;
  attachInterrupt(PIN_IR_RX, irIsr, CHANGE);
  uint32_t t0 = millis();
  String line;
  while (millis() - t0 < 10000) {
    if (pollLine(line) && (line == "s" || line == "q")) { if (line == "q") guidedAbort = true; break; }
    delay(20);
  }
  detachInterrupt(PIN_IR_RX);
  uint32_t n = irEdges;
  spiDeselectAll();
  result("ir-rx", n > 20 ? ST_PASS : ST_WARN, "%lu edges on GPIO%d (idle=%d)", (unsigned long)n, PIN_IR_RX, idle);
}

static void guidedSD() {
  pinMode(PIN_SD_CD, INPUT_PULLUP);
  int before = digitalRead(PIN_SD_CD);
  screenPrompt("SD CARD", "Insert or remove", "the microSD card now (15 s)");
  out("[do] Insert (or remove) the microSD card now. 15 s. 's' skips");
  uint32_t t0 = millis();
  int now = before;
  String line;
  while (millis() - t0 < 15000 && now == before) {
    if (pollLine(line) && (line == "s" || line == "q")) { if (line == "q") guidedAbort = true; break; }
    now = digitalRead(PIN_SD_CD);
    delay(20);
  }
  if (now != before) {
    delay(500);
    result("sd-detect", ST_PASS, "GPIO%d %d>%d on change", PIN_SD_CD, before, now);
    testSD();
  } else {
    result("sd-detect", ST_WARN, "GPIO%d stayed %d", PIN_SD_CD, before);
  }
}

static void runGuided() {
  guidedAbort = false;
  out("[probe] guided test start. 'q' aborts, 's' skips a step");
  guidedButtons();
  if (!guidedAbort) guidedLCD();
  if (!guidedAbort) guidedBacklight();
  if (!guidedAbort) guidedTouch();
  if (!guidedAbort) guidedLEDs();
  if (!guidedAbort) guidedBuzzer();
  if (!guidedAbort) guidedIR();
  if (!guidedAbort) guidedSD();
  screenSummary();
  out("[probe] guided test %s", guidedAbort ? "aborted" : "complete");
}

// ------------------------------------------------------------------ auto suite

static void autoStep(const char* name) {
  tft.fillRect(0, tft.height() - 30, tft.width(), 30, TFT_BLACK);
  tft.setFont(&fonts::Font2);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setTextDatum(textdatum_t::top_center);
  tft.drawString(String("now: ") + name, tft.width() / 2, tft.height() - 26);
  tft.setTextDatum(textdatum_t::top_left);
}

static void runAuto() {
  screenPrompt("FUD PROBE", "Auto tests", "radios, bus, power. ~15 s");
  autoStep("chip info"); testInfo();
  autoStep("I2C scan"); testI2C();
  autoStep("buttons chip"); testPCF();
  autoStep("battery"); testIP5306(); testVbatAdc();
  autoStep("I2C speed"); testI2CSpeed();
  autoStep("free pins"); testFreePins();
  autoStep("SD card"); testSD();
  autoStep("SPI bus"); spiDiag();
  const char* nn[3] = {"nRF24 #1", "nRF24 #2", "nRF24 #3"};
  for (int i = 0; i < 3; i++) { autoStep(nn[i]); testNrf(i, 3); }
  autoStep("CC1101"); testCC1101();
  autoStep("Wi-Fi scan"); testWiFi();
  autoStep("BLE scan"); testBLE();
  screenSummary();
}

// ------------------------------------------------------------------ watch mode

static void runWatch(int secs) {
  out("[watch] %d s: buttons, touch, SD CD, IR RX, free pins. any line stops", secs);
  screenPrompt("WATCH", "Poke anything", "events stream to serial");
  freeWatchStart();
  uint8_t lastFree = freeSnapshot();
  int lastPcf = pcfRead();
  pinMode(PIN_SD_CD, INPUT_PULLUP);
  int lastCd = digitalRead(PIN_SD_CD);
  spiDeselectAll();
  pinMode(PIN_IR_RX, INPUT_PULLUP);
  irEdges = 0;
  attachInterrupt(PIN_IR_RX, irIsr, CHANGE);
  uint32_t lastIr = 0, t0 = millis(), lastTouch = 0;
  String line;
  while (millis() - t0 < (uint32_t)secs * 1000) {
    if (pollLine(line)) break;
    int v = pcfRead();
    if (v != lastPcf) { out("[watch] pcf 0x%02X > 0x%02X", lastPcf & 0xFF, v & 0xFF); lastPcf = v; }
    uint8_t f = freeSnapshot();
    if (f != lastFree) {
      for (size_t i = 0; i < sizeof(FREE_PINS); i++)
        if ((f ^ lastFree) & (1 << i)) out("[watch] GPIO%d > %d", FREE_PINS[i], (f >> i) & 1);
      lastFree = f;
    }
    int cd = digitalRead(PIN_SD_CD);
    if (cd != lastCd) { out("[watch] SD CD > %d", cd); lastCd = cd; }
    if (irEdges != lastIr && millis() - t0 > 0) { out("[watch] IR edges %lu", (unsigned long)irEdges); lastIr = irEdges; delay(100); }
    lgfx::touch_point_t tp;
    if (tft.getTouchRaw(&tp, 1) && millis() - lastTouch > 150) {
      int32_t x, y;
      tft.getTouch(&x, &y);
      out("[watch] touch raw(%d,%d) screen(%ld,%ld)", tp.x, tp.y, (long)x, (long)y);
      tft.fillCircle(x, y, 2, TFT_GREEN);
      lastTouch = millis();
    }
    delay(5);
  }
  detachInterrupt(PIN_IR_RX);
  spiDeselectAll();
  screenSummary();
}

// ------------------------------------------------------------------ screenshot

// Same wire format as the squachwatch dev-tools shot.py: RGB332 hex rows.
static void runShot() {
  int w = tft.width(), h = tft.height();
  static lgfx::rgb565_t row[320];
  static char hex[2 * 320 + 16];
  reliableOut = true;
  out("[shot] BEGIN %d %d 0", w, h);
  for (int y = 0; y < h; y++) {
    tft.readRect(0, y, w, 1, row);
    int n = snprintf(hex, sizeof(hex), "R%d:", y);
    for (int x = 0; x < w; x++) {
      uint8_t v = (row[x].R8() & 0xE0) | ((row[x].G8() & 0xE0) >> 3) | (row[x].B8() >> 6);
      static const char* hx = "0123456789ABCDEF";
      hex[n++] = hx[v >> 4];
      hex[n++] = hx[v & 15];
    }
    hex[n] = 0;
    emit(hex);
  }
  out("[shot] END");
  reliableOut = false;
}

// ------------------------------------------------------------------ commands

static void help() {
  out("FUD PROBE " PROBE_VERSION " commands (all read/receive only):");
  out("  auto              run all automatic tests");
  out("  guided            buttons, LCD, backlight, touch, LEDs, speaker, IR, SD (needs a human)");
  out("  report            print all results");
  out("  info | i2c | i2cspeed | pcf | bat | pins | sd | spidiag | cc | wifi | ble");
  out("  nrf N [passes]    test radio N (1-3); 'nrfscan N [passes]' draws a spectrum");
  out("  cetest            drive each nRF24 CE high and read it back (finds clamped lines)");
  out("  i2cr ADDR REG [n] read I2C registers (hex)");
  out("  ccreg REG         read a CC1101 register (hex)");
  out("  watch [secs]      stream button/touch/pin/IR/CD events");
  out("  buttons | lcd | backlight | touch | leds | speaker | ir | sdcd   single guided steps");
  out("  rot N | invert 0/1 | bgr 0/1 | bl 0-255   display tweaks");
  out("  led R G B [i]     set NeoPixels (all, or index i)");
  out("  tone HZ MS        play a tone on the speaker");
  out("  shot              dump screen (use tools/probe.py shot out.png)");
  out("  summary           redraw result list on screen");
  out("  events            print the background input log (buttons, touch, IR, SD, pins)");
  out("  i2cid [secs]      hold SCL low, then SDA low (default 30 s each) to find them with a meter");
  out("  ledtest | ledoff  hold NeoPixels at 0 red 1 green 2 blue 3 white | off");
  out("  ledcore [R G B]   first pixel via the core driver (default red)");
  out("  bars              hold color bars on screen until the next command");
}

static void report() {
  out("[report] %d results", (int)results.size());
  for (auto& r : results) out("[report] %-10s %s %s", r.name.c_str(), ST_NAME[r.st], r.detail.c_str());
}

static void recorderArm();
static void recorderDisarm();
static void printEvents();
static void showLedPattern();
static void showBars();
static void drawEvents();

static void handle(String line) {
  recorderDisarm();
  int sp = line.indexOf(' ');
  String cmd = sp < 0 ? line : line.substring(0, sp);
  String args = sp < 0 ? "" : line.substring(sp + 1);
  long a[4] = {-1, -1, -1, -1};
  {
    int n = 0;
    String rest = args;
    while (rest.length() && n < 4) {
      int s = rest.indexOf(' ');
      String tok = s < 0 ? rest : rest.substring(0, s);
      bool isHex = cmd == "i2cr" || cmd == "ccreg";
      a[n++] = strtol(tok.c_str(), nullptr, isHex ? 16 : 10);
      rest = s < 0 ? "" : rest.substring(s + 1);
      rest.trim();
    }
  }
  guidedAbort = false;
  if (cmd == "help" || cmd == "?") help();
  else if (cmd == "auto") runAuto();
  else if (cmd == "guided") runGuided();
  else if (cmd == "report") report();
  else if (cmd == "summary") screenSummary();
  else if (cmd == "info") testInfo();
  else if (cmd == "i2c") testI2C();
  else if (cmd == "pcf") testPCF();
  else if (cmd == "bat") { testIP5306(); testVbatAdc(); }
  else if (cmd == "i2cspeed") testI2CSpeed();
  else if (cmd == "pins") testFreePins();
  else if (cmd == "sd") testSD();
  else if (cmd == "spidiag") spiDiag();
  else if (cmd == "cc") testCC1101();
  else if (cmd == "wifi") testWiFi();
  else if (cmd == "ble") testBLE();
  else if (cmd == "nrf" && a[0] >= 1 && a[0] <= 3) testNrf(a[0] - 1, a[1] > 0 ? a[1] : 3);
  else if (cmd == "nrfscan" && a[0] >= 1 && a[0] <= 3) nrfScanPrint(a[0] - 1, a[1] > 0 ? a[1] : 20);
  else if (cmd == "i2cr" && a[0] > 0) {
    int n = a[2] > 0 ? a[2] : 1;
    for (int i = 0; i < n; i++) out("[i2cr] 0x%02lX reg %02lX = %d", a[0], a[1] + i, i2cReadReg(a[0], a[1] + i));
  }
  else if (cmd == "ccreg" && a[0] >= 0) { spiDeselectAll(); out("[cc] reg %02lX = %02X", a[0], a[0] >= 0x30 ? ccStatus(a[0]) : ccReadReg(a[0])); }
  else if (cmd == "watch") runWatch(a[0] > 0 ? a[0] : 30);
  else if (cmd == "buttons") { guidedButtons(); screenSummary(); }
  else if (cmd == "lcd") { guidedLCD(); screenSummary(); }
  else if (cmd == "backlight") { guidedBacklight(); screenSummary(); }
  else if (cmd == "touch") { guidedTouch(); screenSummary(); }
  else if (cmd == "leds") { guidedLEDs(); screenSummary(); }
  else if (cmd == "speaker") { guidedBuzzer(); screenSummary(); }
  else if (cmd == "ir") { guidedIR(); screenSummary(); }
  else if (cmd == "sdcd") { guidedSD(); screenSummary(); }
  else if (cmd == "rot" && a[0] >= 0) { tft.setRotation(a[0] & 7); screenSummary(); }
  else if (cmd == "invert" && a[0] >= 0) { tft.invertDisplay(a[0]); }
  else if (cmd == "bgr" && a[0] >= 0) {
    auto c = tft.panel().config(); c.rgb_order = a[0]; tft.panel().config(c); tft.init(); tft.setRotation(PROBE_ROTATION); screenSummary();
  }
  else if (cmd == "bl" && a[0] >= 0) tft.setBrightness(a[0]);
  else if (cmd == "led" && a[2] >= 0) {
    if (a[3] >= 0) ledsSet(a[3], a[0], a[1], a[2]);
    else ledsFill(a[0], a[1], a[2]);
    ledsShow();
  }
  else if (cmd == "tone" && a[0] > 0) { tone(PIN_BUZZER, a[0], a[1] > 0 ? a[1] : 200); delay(a[1] > 0 ? a[1] : 200); noTone(PIN_BUZZER); }
  else if (cmd == "shot") runShot();
  else if (cmd == "events") printEvents();
  else if (cmd == "cetest") ceTest();
  else if (cmd == "i2cid") i2cIdentify(a[0] > 0 ? a[0] : 30);
  else if (cmd == "ledtest") showLedPattern();
  else if (cmd == "ledoff") ledsClear();
  else if (cmd == "ledcore") {  // core RMT driver, first pixel only: rules the library in or out
    rgbLedWrite(PIN_NEOPIXEL, a[0] >= 0 ? a[0] : 255, a[1] >= 0 ? a[1] : 0, a[2] >= 0 ? a[2] : 0);
    ledInit = true;  // rgbLedWrite set the pin up for RMT at the same 10 MHz
    result("ledcore", ST_INFO, "rgbLedWrite GPIO%d first pixel", PIN_NEOPIXEL);
  }
  else if (cmd == "bars") showBars();
  else out("[probe] unknown or bad args: %s  (try help)", line.c_str());
  recorderArm();
  drawEvents();
  out("[probe] DONE %s", cmd.c_str());
}

// ------------------------------------------------------------------ main

void setup() {
  Serial.begin(115200);
  // Never 0: HWCDC::write counts "tries" down from this value as uint32, so 0 wraps
  // to ~49 days and the first full buffer with no reader hangs forever.
  Serial.setTxTimeoutMs(20);
  Serial0.begin(115200);
  spiDeselectAll();
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);
  ledsClear();
  tft.init();
  tft.setRotation(PROBE_ROTATION);
  tft.setBrightness(255);
  screenPrompt("FUD PROBE " PROBE_VERSION, "Booting", "read-only hardware probe");
  Wire.begin(PIN_SDA, PIN_SCL, 100000);
  SPI.begin(PIN_SPI_SCK, PIN_SPI_MISO, PIN_SPI_MOSI, -1);
  delay(1500);  // give a host time to open the port
  out("[probe] FUD PROBE " PROBE_VERSION " boot");
  runAuto();
  recorderArm();
  drawEvents();
  out("[probe] READY  (type help)");
}

// ------------------------------------------------------------------ background event recorder

// Records inputs whenever they happen, so a human can act later and we read the log after.
struct Ev { uint32_t ms; char txt[48]; };
static Ev evs[64];
static int evNext = 0, evTotal = 0;
static bool recArmed = false;

static void drawEvents() {
  if (!onSummary) return;
  const int top = 226, lines = 8;
  tft.fillRect(0, top - 12, tft.width(), 12 + lines * 10, TFT_BLACK);
  tft.setFont(&fonts::Font0);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString("recent inputs:", 2, top - 11);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  int n = evTotal < lines ? evTotal : lines;
  for (int i = 0; i < n; i++) {
    const Ev& e = evs[(evNext - n + i + 64) % 64];
    char line[64];
    snprintf(line, sizeof(line), "%5lus %s", (unsigned long)(e.ms / 1000), e.txt);
    tft.drawString(line, 2, top + i * 10);
  }
}

static void addEvent(const char* fmt, ...) {
  Ev& e = evs[evNext];
  e.ms = millis();
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(e.txt, sizeof(e.txt), fmt, ap);
  va_end(ap);
  evNext = (evNext + 1) % 64;
  evTotal++;
  out("[ev] %lus %s", (unsigned long)(e.ms / 1000), e.txt);
  drawEvents();
}

static void recorderArm() {
  freeWatchStart();
  pinMode(PIN_SD_CD, INPUT_PULLUP);
  pinMode(PIN_IR_RX, INPUT_PULLUP);  // nRF3 CSN floats high via TSOP idle; radios are idle here
  irEdges = 0;
  attachInterrupt(PIN_IR_RX, irIsr, CHANGE);
  recArmed = true;
}

static void recorderDisarm() {
  if (!recArmed) return;
  detachInterrupt(PIN_IR_RX);
  spiDeselectAll();
  recArmed = false;
}

static void eventTick() {
  static uint32_t last = 0;
  static int lastPcf = -2, lastCd = -1;
  static uint8_t lastFree = 0xFF;
  static uint32_t pressMs[8];
  static uint32_t irSeen = 0, irLastChange = 0;
  static bool touching = false;
  static long tsx = 0, tsy = 0;
  static int tn = 0;
  if (!recArmed || millis() - last < 10) return;
  last = millis();

  int v = pcfRead();
  if (v >= 0 && lastPcf >= 0 && v != lastPcf) {
    for (int b = 0; b < 8; b++) {
      bool was = lastPcf & (1 << b), now = v & (1 << b);
      if (was && !now) pressMs[b] = millis();
      if (!was && now) addEvent("button P%d (%lums)", b, (unsigned long)(millis() - pressMs[b]));
    }
  }
  if (v >= 0) lastPcf = v;

  lgfx::touch_point_t tp;
  if (tft.getTouchRaw(&tp, 1)) { touching = true; tsx += tp.x; tsy += tp.y; tn++; }
  else if (touching) {
    touching = false;
    if (tn) addEvent("touch raw %ld,%ld (%d samples)", tsx / tn, tsy / tn, tn);
    tsx = tsy = 0; tn = 0;
  }

  int cd = digitalRead(PIN_SD_CD);
  if (lastCd >= 0 && cd != lastCd) addEvent("SD card %s (CD=%d)", cd ? "removed" : "inserted", cd);
  lastCd = cd;

  uint8_t f = freeSnapshot();
  if (lastFree != 0xFF && f != lastFree)
    for (size_t i = 0; i < sizeof(FREE_PINS); i++)
      if ((f ^ lastFree) & (1 << i)) addEvent("GPIO%d > %d", FREE_PINS[i], (f >> i) & 1);
  lastFree = f;

  uint32_t ir = irEdges;
  if (ir != irSeen) { irSeen = ir; irLastChange = millis(); }
  else if (ir > 0 && millis() - irLastChange > 300) {
    addEvent("IR burst %lu edges", (unsigned long)ir);
    irEdges = 0; irSeen = 0;
  }
}

static void printEvents() {
  int n = evTotal < 64 ? evTotal : 64;
  out("[events] %d recorded (last %d shown)", evTotal, n);
  for (int i = 0; i < n; i++) {
    const Ev& e = evs[(evNext - n + i + 64) % 64];
    out("[events] %6lus %s", (unsigned long)(e.ms / 1000), e.txt);
  }
}

// Static patterns that stay up until the next command, for a human to look at later.
static void showLedPattern() {
  ledsSet(0, 255, 0, 0); ledsSet(1, 0, 255, 0); ledsSet(2, 0, 0, 255); ledsSet(3, 255, 255, 255);
  ledsShow();
  result("leds", ST_INFO, "pattern on: 0 red 1 green 2 blue 3 white");
}

static void showBars() {
  int w = tft.width(), h = tft.height();
  screenHeader("LCD COLORS");
  int bh = (h - 18) / 5;
  uint16_t c[5] = {TFT_RED, TFT_GREEN, TFT_BLUE, TFT_WHITE, TFT_BLACK};
  const char* n[5] = {"RED", "GREEN", "BLUE", "WHITE", "BLACK"};
  tft.setFont(&fonts::Font2);
  for (int i = 0; i < 5; i++) {
    tft.fillRect(0, 18 + i * bh, w, bh, c[i]);
    tft.setTextColor(i == 4 ? TFT_WHITE : TFT_BLACK, c[i]);
    tft.drawString(n[i], 4, 22 + i * bh);
  }
  tft.drawRect(0, 18 + 4 * bh, w, bh, TFT_DARKGREY);
}

// Live power readout on the results screen, so it can be read with USB unplugged.
static void liveFooter() {
  static uint32_t last = 0;
  if (!onSummary || millis() - last < 1000) return;
  last = millis();
  pinMode(PIN_BUZZER, INPUT);
  uint32_t sum = 0;
  for (int i = 0; i < 16; i++) sum += analogReadMilliVolts(PIN_BUZZER);
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);
  int r70 = ip5306Present ? i2cReadReg(0x75, 0x70) : -1;
  int r78 = ip5306Present ? i2cReadReg(0x75, 0x78) : -1;
  char buf[64];
  snprintf(buf, sizeof(buf), "IO2 %4lumV  usb=%d  lvl=%02X  %lus  ", (unsigned long)(sum / 16),
           r70 >= 0 ? ((r70 >> 3) & 1) : -1, r78 & 0xFF, (unsigned long)(millis() / 1000));
  tft.setFont(&fonts::Font0);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawString(buf, 80, tft.height() - 10);
}

void loop() {
  String line;
  if (pollLine(line)) handle(line);
  eventTick();
  liveFooter();
  delay(2);
}
