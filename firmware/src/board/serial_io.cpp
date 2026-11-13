#include "serial_io.h"
#include <stdarg.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace sio {

static bool cdcStalled = false;
static bool reliable = false;
static String lineBuf[2];
// Both cores log (UI on 1, scanner on 0). Without a lock their writes collide on the
// USB driver lock, time out, and the stall guard starts dropping lines.
static SemaphoreHandle_t outLock = nullptr;

// USB CDC drops what does not fit, so retry, but only as long as the caller allows.
static bool writeAll(Print& p, const uint8_t* d, size_t n, uint32_t timeoutMs) {
  uint32_t start = millis();
  while (n) {
    size_t w = p.write(d, n);
    if (w == 0) {
      if (millis() - start >= timeoutMs) return false;
      delay(1);
      continue;
    }
    d += w;
    n -= w;
  }
  return true;
}

// USB plugged into a computer with no terminal open still reads as connected, but nobody
// drains the buffer. After one stall, stop waiting on USB until a write gets through again.
static void emitTo(Print& p, const char* s, size_t n, bool isCdc) {
  uint32_t t = reliable ? 3000 : (isCdc && cdcStalled ? 0 : 100);
  bool ok = writeAll(p, (const uint8_t*)s, n, t) && writeAll(p, (const uint8_t*)"\n", 1, t);
  if (isCdc) cdcStalled = !ok;
}

void begin() {
  Serial.setTxBufferSize(2048);   // default 256 B overflows when both cores log at once
  Serial.begin(115200);
  // Never 0: HWCDC::write counts "tries" down from this value as uint32, so 0 wraps
  // to ~49 days and the first full buffer with no reader hangs forever.
  Serial.setTxTimeoutMs(20);
  Serial0.begin(115200);
  outLock = xSemaphoreCreateMutex();
}

void raw(const char* s) {
  size_t n = strlen(s);
  if (outLock) xSemaphoreTake(outLock, portMAX_DELAY);
  if (Serial) emitTo(Serial, s, n, true);
  emitTo(Serial0, s, n, false);
  if (outLock) xSemaphoreGive(outLock);
}

void out(const char* fmt, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  raw(buf);
}

bool readLine(String& line) {
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
      if (lineBuf[i].length() < 120) lineBuf[i] += c;
    }
  }
  return false;
}

void setReliable(bool on) { reliable = on; }

}  // namespace sio
