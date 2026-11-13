#include "scanner.h"
#include <NimBLEDevice.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include "../board/serial_io.h"
#include "detections.h"

// signatures.h and user_ignore.h are FUD's files, kept verbatim. They expect FUD's names.
using Cat = det::Cat;
static constexpr Cat CAT_TRACKER = det::TRACKER, CAT_CAMERA = det::CAMERA, CAT_DRONE = det::DRONE,
                     CAT_SUBGHZ = det::SUBGHZ, CAT_HACKDEV = det::HACKDEV, CAT_ATTACK = det::ATTACK;
#include "signatures.h"
#include "user_ignore.h"

namespace scanner {

static volatile char radio = '-';
static volatile bool pauseReq = false;
static volatile uint32_t nCycles = 0;

char activeRadio() { return radio; }
void pause(bool on) { pauseReq = on; }
const char* ouiLabel(const uint8_t* mac) {
  for (int k = 0; k < OUI_SIGS_N; k++)
    if (!memcmp(mac, OUI_SIGS[k].oui, 3)) return OUI_SIGS[k].label;
  return nullptr;
}
bool paused() { return pauseReq; }
uint32_t cycles() { return nCycles; }

// ---------------------------------------------------------------- compiled-in ignores

static bool ignoredBleName(const char* nm) {
  if (!nm || !nm[0]) return false;
  for (int i = 0; i < USER_IGNORE_BLE_NAMES_N; i++)
    if (strstr(nm, USER_IGNORE_BLE_NAMES[i])) return true;
  return false;
}
static bool ignoredSsid(const char* s) {
  for (int i = 0; i < USER_IGNORE_SSIDS_N; i++)
    if (!strcmp(USER_IGNORE_SSIDS[i], s)) return true;
  return false;
}
static bool ignoredMac(const uint8_t* m) {
  for (int i = 0; i < USER_IGNORE_MACS_N; i++)
    if (!memcmp(USER_IGNORE_MACS[i], m, 6)) return true;
  return false;
}

// ---------------------------------------------------------------- BLE

// BACKPORT(FUD) 2026-11-13: a 16-bit UUID in the advertised service list OR as a service
// data UUID. Trackers (Tile, Chipolo, DULT) often only send service data.
static bool hasSvc(const NimBLEAdvertisedDevice* dev, uint16_t u) {
  NimBLEUUID id(u);
  if (dev->isAdvertisingService(id)) return true;
  for (int i = 0; i < (int)dev->getServiceDataCount(); i++)
    if (dev->getServiceDataUUID(i) == id) return true;
  return false;
}

static void classifyBle(const NimBLEAdvertisedDevice* dev) {
  uint8_t key[6];
  memcpy(key, dev->getAddress().getBase()->val, 6);
  if (ignoredMac(key)) return;
  int8_t rssi = dev->getRSSI();
  std::string nm = dev->getName();
  std::string md = dev->getManufacturerData();
  uint16_t company = md.size() >= 2 ? ((uint8_t)md[0] | ((uint8_t)md[1] << 8)) : 0xFFFF;
  const char* name = nm.c_str();
  if (ignoredBleName(name)) return;
  // Hacker devices (active): Flipper by name
  bool flip = nm.find("Flipper") != std::string::npos;
  for (int i = 0; !flip && i < FLIPPER_SVC_N; i++) flip = hasSvc(dev, FLIPPER_SVC0 + i);
  if (flip) {
    det::enq(key, CAT_HACKDEV, "Flipper", name, rssi, 'B', false, true, 'H');
    return;
  }
  // Meta/Ray-Ban glasses: Luxottica CID + Meta service together (phones with Meta apps
  // advertise the service alone).
  if ((company == META_CID || company == META_CID2) && hasSvc(dev, META_SVC)) {
    det::enq(key, CAT_CAMERA, "Meta glass", name, rssi, 'B', false, false, 'H');
    return;
  }
  // Apple Find My / AirTag: company 0x004C, offline-finding type byte 0x12
  if (company == APPLE_CID && md.size() >= 3 && (uint8_t)md[2] == FINDMY_TYPE) {
    det::enq(key, CAT_TRACKER, "AirTag", name, rssi, 'B', false, false, 'H', true);  // bundled
    return;
  }
  for (int i = 0; i < BLE_SIGS_N; i++) {
    const BleSig& s = BLE_SIGS[i];
    bool svcOk = s.service == 0xFFFF || hasSvc(dev, s.service);
    bool cidOk = s.company == 0xFFFF || company == s.company;
    if (svcOk && cidOk) {
      det::enq(key, s.cat, s.label, name, rssi, 'B', false, false, s.conf);
      return;
    }
  }
}

// ---------------------------------------------------------------- Wi-Fi networks

// All BSSIDs of one network collapse to one entry (FNV-1a hash of the SSID).
static void ssidKey(const char* s, uint8_t k[6]) {
  uint32_t h = 2166136261u;
  for (const char* p = s; *p; p++) { h ^= (uint8_t)*p; h *= 16777619u; }
  k[0] = 0xE7; k[1] = h; k[2] = h >> 8; k[3] = h >> 16; k[4] = h >> 24; k[5] = (uint8_t)strlen(s);
}

// Where am I, without GPS: the strongest access points around. When most of them have
// changed for two scans in a row, the user has moved, and det gets a new place number.
// A tracker only counts as following you once it has been seen in two places.
static const int PLACE_N = 12;
static uint32_t placeRef[PLACE_N];
static int placeRefN = 0, placeLow = 0;
static uint16_t placeNo = 0;
static void placeUpdate(int n) {
  uint32_t cur[PLACE_N];
  int8_t rs[PLACE_N];
  int cn = 0;
  for (int i = 0; i < n; i++) {   // keep the strongest PLACE_N
    uint8_t* b = WiFi.BSSID(i);
    if (!b) continue;
    uint32_t h = 2166136261u;
    for (int k = 0; k < 6; k++) { h ^= b[k]; h *= 16777619u; }
    int8_t r = WiFi.RSSI(i);
    if (cn < PLACE_N) { cur[cn] = h; rs[cn++] = r; continue; }
    int w = 0;
    for (int k = 1; k < cn; k++) if (rs[k] < rs[w]) w = k;
    if (r > rs[w]) { cur[w] = h; rs[w] = r; }
  }
  if (cn < 3) return;   // too few to tell places apart
  if (!placeRefN) {
    memcpy(placeRef, cur, cn * 4);
    placeRefN = cn;
    det::setPlace(placeNo, true);
    return;
  }
  int hit = 0;
  for (int i = 0; i < cn; i++)
    for (int k = 0; k < placeRefN; k++) if (cur[i] == placeRef[k]) { hit++; break; }
  if (hit * 4 > cn) { placeLow = 0; return; }   // over a quarter still here: same place
  if (++placeLow < 2) return;
  placeLow = 0;
  memcpy(placeRef, cur, cn * 4);
  placeRefN = cn;
  placeNo++;
  det::setPlace(placeNo, true);
  sio::out("[scan] new place %u (%d of %d access points known)", (unsigned)placeNo, hit, cn);
}

static void scanWifiOnce() {
  // Passive: listen for beacons only, 120 ms per channel. Sends no probe requests.
  int n = WiFi.scanNetworks(false, true, true, 120);
  for (int i = 0; i < n; i++) {
    String ss = WiFi.SSID(i);
    if (ss.length() == 0 || ignoredSsid(ss.c_str())) continue;
    uint8_t* b = WiFi.BSSID(i);
    if (!b) continue;
    uint8_t key[6];
    memcpy(key, b, 6);
    if (ignoredMac(key)) continue;
    // Evil twin: the same SSID both OPEN and secured. Encryption-only differences
    // (WPA2 vs WPA3) are normal transition setups, not flagged.
    bool iOpen = WiFi.encryptionType(i) == WIFI_AUTH_OPEN, twin = false;
    for (int j = 0; j < n; j++)
      if (j != i && WiFi.SSID(j) == ss && (WiFi.encryptionType(j) == WIFI_AUTH_OPEN) != iOpen) { twin = true; break; }
    if (twin) {
      uint8_t tk[6];
      ssidKey(ss.c_str(), tk);
      det::enq(tk, CAT_ATTACK, "evil twin", ss.c_str(), WiFi.RSSI(i), 'W', true, false, 'M');
      continue;
    }
    if (ss == "pwned") {
      det::enq(key, CAT_HACKDEV, "pwned", ss.c_str(), WiFi.RSSI(i), 'W', true, true, 'H');
      continue;
    }
    bool matched = false;
    for (int k = 0; k < SSID_SIGS_N; k++) {
      if (!strncasecmp(ss.c_str(), SSID_SIGS[k].prefix, strlen(SSID_SIGS[k].prefix))) {
        det::enq(key, SSID_SIGS[k].cat, SSID_SIGS[k].label, ss.c_str(), WiFi.RSSI(i), 'W', true, SSID_SIGS[k].active, 'M');
        matched = true;
        break;
      }
    }
    if (!matched)
      for (int k = 0; k < OUI_SIGS_N; k++)
        if (!memcmp(key, OUI_SIGS[k].oui, 3)) {
          det::enq(key, OUI_SIGS[k].cat, OUI_SIGS[k].label, ss.c_str(), WiFi.RSSI(i), 'W', false, false, OUI_SIGS[k].conf);
          break;
        }
  }
  placeUpdate(n);
  WiFi.scanDelete();
}

// ---------------------------------------------------------------- Wi-Fi deauth monitor

// Deauth trips the alarm (disassoc is usually roaming churn). A real flood is many, fast.
static const uint32_t DEAUTH_THRESH = 10;
static volatile uint32_t gDeauth = 0, gDisassoc = 0;
static volatile uint8_t gSrc[6] = {0};
static volatile uint8_t gBcast = 0;
static volatile uint16_t gReason = 0;

static void promiscCb(void* buf, wifi_promiscuous_pkt_type_t type) {
  if (type != WIFI_PKT_MGMT) return;
  const uint8_t* f = ((wifi_promiscuous_pkt_t*)buf)->payload;
  uint8_t fc = f[0], ftype = (fc & 0x0C) >> 2, sub = (fc & 0xF0) >> 4;
  if (ftype != 0) return;
  if (sub == 10) { gDisassoc++; return; }
  if (sub == 12) {
    gDeauth++;
    for (int i = 0; i < 6; i++) gSrc[i] = f[10 + i];   // addr2 = transmitter
    bool bc = true;
    for (int i = 0; i < 6; i++) if (f[4 + i] != 0xFF) bc = false;
    if (bc) gBcast = 1;
    gReason = (uint16_t)f[24] | ((uint16_t)f[25] << 8);
  }
}

static void monitorDeauth() {
  gDeauth = gDisassoc = gBcast = 0;
  gReason = 0;
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_promiscuous_rx_cb(&promiscCb);
  static const uint8_t chans[] = {1, 6, 11, 1, 6, 11};
  for (uint8_t c : chans) {
    if (pauseReq) break;
    esp_wifi_set_channel(c, WIFI_SECOND_CHAN_NONE);
    vTaskDelay(pdMS_TO_TICKS(900));
  }
  esp_wifi_set_promiscuous(false);
  uint8_t src[6];
  for (int i = 0; i < 6; i++) src[i] = gSrc[i];
  sio::out("[scan] deauth=%u disassoc=%u bcast=%u reason=%u", (unsigned)gDeauth, (unsigned)gDisassoc,
           (unsigned)gBcast, (unsigned)gReason);
  if (gDeauth >= DEAUTH_THRESH) {
    // Sources are usually spoofed: one fixed key per flood, not one alarm per fake MAC.
    static const uint8_t dkey[6] = {0xDE, 0xAD, 0xBE, 0xEF, 0xDE, 0xAD};
    char nm[32];
    snprintf(nm, sizeof(nm), "%02X%02X%02X d%u r%u%s", src[0], src[1], src[2], (unsigned)gDeauth,
             (unsigned)gReason, gBcast ? " B" : "");
    det::enq(dkey, CAT_ATTACK, "deauth", nm, -50, 'W', false, true, 'H');
  }
}

// ---------------------------------------------------------------- task

// BLE results arrive on NimBLE's host task through this callback.
static volatile int bleSeen = 0;
static volatile bool finderOn = false;
static volatile bool ssidOn = false;
static portMUX_TYPE ssMux = portMUX_INITIALIZER_UNLOCKED;
static ScanNet ssBuf[40];
static int ssBufN = 0;
static volatile uint32_t ssSeq = 0;

void setSsidMode(bool on) { ssidOn = on; }
uint32_t ssidResults(ScanNet* out, int max, int& n) {
  portENTER_CRITICAL(&ssMux);
  n = min(max, ssBufN);
  memcpy(out, ssBuf, sizeof(ScanNet) * n);
  uint32_t s = ssSeq;
  portEXIT_CRITICAL(&ssMux);
  return s;
}
static uint8_t finderMac[6];
static volatile int8_t fRssi = -127;
static volatile uint32_t fSeen = 0;

void setFinderTarget(const uint8_t* mac) {
  if (mac) { memcpy(finderMac, mac, 6); fSeen = 0; fRssi = -127; finderOn = true; }
  else finderOn = false;
}
bool finderReading(int8_t& rssi, uint32_t& age) {
  if (!fSeen) return false;
  rssi = fRssi;
  age = millis() - fSeen;
  return true;
}

class BleCb : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* d) override {
    bleSeen++;
    if (finderOn) {   // hunting: only the target matters, and every advert counts
      if (!memcmp(d->getAddress().getBase()->val, finderMac, 6)) { fRssi = d->getRSSI(); fSeen = millis(); }
      return;
    }
    classifyBle(d);
  }
};
static BleCb bleCb;
static volatile bool saverMode = false;
static volatile bool bleOn = false, wifiOn = false;

void setSaver(bool on) { saverMode = on; }
bool bleActive() { return bleOn; }
bool wifiActive() { return wifiOn; }

static void deauthStart() {
  gDeauth = gDisassoc = gBcast = 0;
  gReason = 0;
  wifi_promiscuous_filter_t f = {WIFI_PROMIS_FILTER_MASK_MGMT};
  esp_wifi_set_promiscuous_filter(&f);
  esp_wifi_set_promiscuous_rx_cb(&promiscCb);
  esp_wifi_set_promiscuous(true);
}

static void deauthJudge() {
  uint8_t src[6];
  for (int i = 0; i < 6; i++) src[i] = gSrc[i];
  if (gDeauth >= DEAUTH_THRESH) {
    static const uint8_t dkey[6] = {0xDE, 0xAD, 0xBE, 0xEF, 0xDE, 0xAD};
    char nm[32];
    snprintf(nm, sizeof(nm), "%02X%02X%02X d%u r%u%s", src[0], src[1], src[2], (unsigned)gDeauth,
             (unsigned)gReason, gBcast ? " B" : "");
    det::enq(dkey, CAT_ATTACK, "deauth", nm, -50, 'W', false, true, 'H');
  }
  gDeauth = gDisassoc = gBcast = 0;
}

// Full power: BLE and Wi-Fi together through the ESP32's radio coexistence. BLE scans
// continuously (restarted every 6 s to refresh its duplicate filter), the Wi-Fi monitor hops
// 1/6/11 alongside it, and a passive network scan runs every 30 s.
static void cycleConcurrent(NimBLEScan* s) {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  static const uint8_t chans[] = {1, 6, 11};
  uint32_t lastNet = 0;
  int hop = 0;
  deauthStart();
  wifiOn = bleOn = true;
  while (!pauseReq && !saverMode && !finderOn && !ssidOn) {
    bleSeen = 0;
    s->start(6000, false, true);
    uint32_t t0 = millis();
    while (s->isScanning() && !pauseReq && !saverMode && !finderOn && !ssidOn) {   // ~6 s, hopping Wi-Fi meanwhile
      esp_wifi_set_channel(chans[hop++ % 3], WIFI_SECOND_CHAN_NONE);
      vTaskDelay(pdMS_TO_TICKS(600));
    }
    s->stop();
    deauthJudge();
    if (millis() - lastNet > 30000 || lastNet == 0) {
      esp_wifi_set_promiscuous(false);
      scanWifiOnce();
      lastNet = millis();
      deauthStart();
    }
    nCycles++;
    sio::out("[scan] cycle %lu ble=%d (%lus) heap=%lu", (unsigned long)nCycles, (int)bleSeen,
             (unsigned long)((millis() - t0) / 1000), (unsigned long)ESP.getFreeHeap());
  }
  esp_wifi_set_promiscuous(false);
  WiFi.mode(WIFI_OFF);
  wifiOn = bleOn = false;
}

// Battery saver: FUD's rotation. One radio job at a time, then rest.
static void cycleRotate(NimBLEScan* s) {
  radio = 'B';
  bleOn = true;
  WiFi.mode(WIFI_OFF);
  bleSeen = 0;
  s->start(6000, false, true);
  while (s->isScanning() && !pauseReq) vTaskDelay(pdMS_TO_TICKS(100));
  s->stop();
  bleOn = false;
  if (pauseReq) return;
  radio = 'W';
  wifiOn = true;
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  scanWifiOnce();
  monitorDeauth();
  WiFi.mode(WIFI_OFF);
  wifiOn = false;
  radio = '-';
  nCycles++;
  sio::out("[scan] saver cycle %lu ble=%d heap=%lu", (unsigned long)nCycles, (int)bleSeen, (unsigned long)ESP.getFreeHeap());
  vTaskDelay(pdMS_TO_TICKS(4000));
}

static void bleConfigure() {
  NimBLEScan* s = NimBLEDevice::getScan();
  s->setScanCallbacks(&bleCb, false);
  s->setMaxResults(0);     // callbacks only, keep no result list
  // Passive: listen to advertisements only, never send a scan request. FUD-Deck transmits
  // nothing but the TV-B-Gone toy. Flippers are matched by the service UUIDs in their
  // advertisements; names that only arrive in scan responses are not seen.
  s->setActiveScan(false);
  s->setInterval(160);
  s->setWindow(80);
}

// While paused, the Wi-Fi and BLE stacks are shut down so a toy gets their memory.
static volatile bool stacksOffNow = false;
bool stacksDown() { return pauseReq && stacksOffNow; }

static void stacksOff() {
  if (stacksOffNow) return;
  WiFi.mode(WIFI_OFF);
  NimBLEDevice::deinit(true);
  stacksOffNow = true;
  sio::out("[scan] paused, radio stacks off, heap=%lu", (unsigned long)ESP.getFreeHeap());
}
static void stacksOn() {
  if (!stacksOffNow) return;
  NimBLEDevice::init("");
  bleConfigure();
  stacksOffNow = false;
  sio::out("[scan] resumed, heap=%lu", (unsigned long)ESP.getFreeHeap());
}

// Hunt: BLE alone, passive, duplicates on, until the Finder lets go.
static void cycleFinder(NimBLEScan* s) {
  radio = 'B';
  bleOn = true;
  esp_wifi_set_promiscuous(false);
  WiFi.mode(WIFI_OFF);
  s->stop();
  s->setScanCallbacks(&bleCb, true);
  s->setActiveScan(false);
  s->setInterval(100);
  s->setWindow(100);
  s->start(0, false, true);   // run until stopped
  sio::out("[scan] finder hunt on");
  while (finderOn && !pauseReq) vTaskDelay(pdMS_TO_TICKS(100));
  s->stop();
  s->setScanCallbacks(&bleCb, false);
  s->setActiveScan(false);
  s->setInterval(160);
  s->setWindow(80);
  bleOn = false;
  sio::out("[scan] finder hunt off");
}

static void bleConfigure();

// SSID Scan toy: passive network scans on this core, results published for the UI.
static void cycleSsid(NimBLEScan* s) {
  s->stop();
  bleOn = false;
  NimBLEDevice::deinit(true);   // the toy needs Wi-Fi only: give BLE's memory back meanwhile
  radio = 'W';
  esp_wifi_set_promiscuous(false);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  wifiOn = true;
  sio::out("[scan] ssid mode on, heap=%lu", (unsigned long)ESP.getFreeHeap());
  while (ssidOn && !pauseReq) {
    int n = WiFi.scanNetworks(false, true, true, 120);   // passive: beacons only
    ScanNet tmp[40];
    int k = 0;
    for (int i = 0; i < n && k < 40; i++) {
      uint8_t* b = WiFi.BSSID(i);
      if (!b) continue;
      strncpy(tmp[k].ssid, WiFi.SSID(i).c_str(), 32);
      tmp[k].ssid[32] = 0;
      memcpy(tmp[k].bssid, b, 6);
      tmp[k].rssi = WiFi.RSSI(i);
      tmp[k].ch = WiFi.channel(i);
      tmp[k].enc = (uint8_t)WiFi.encryptionType(i);
      k++;
    }
    WiFi.scanDelete();
    portENTER_CRITICAL(&ssMux);
    memcpy(ssBuf, tmp, sizeof(ScanNet) * k);
    ssBufN = k;
    ssSeq++;
    portEXIT_CRITICAL(&ssMux);
    for (int t = 0; t < 25 && ssidOn && !pauseReq; t++) vTaskDelay(pdMS_TO_TICKS(100));
  }
  WiFi.mode(WIFI_OFF);
  wifiOn = false;
  NimBLEDevice::init("");
  bleConfigure();
  sio::out("[scan] ssid mode off, heap=%lu", (unsigned long)ESP.getFreeHeap());
}

static void task(void*) {
  bleConfigure();
  for (;;) {
    if (pauseReq) { radio = '-'; stacksOff(); vTaskDelay(pdMS_TO_TICKS(150)); continue; }
    stacksOn();
    NimBLEScan* s = NimBLEDevice::getScan();
    if (finderOn) cycleFinder(s);
    else if (ssidOn) cycleSsid(s);   // re-inits BLE; the loop re-fetches the scan object
    else if (saverMode) cycleRotate(s);
    else { radio = '*'; cycleConcurrent(s); }
  }
}

void begin() {
  NimBLEDevice::init("");
  // Core 0, low priority. The UI (Arduino loop) owns core 1.
  xTaskCreatePinnedToCore(task, "scan", 8192, nullptr, 1, nullptr, 0);
}

}  // namespace scanner
