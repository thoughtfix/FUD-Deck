// Logging and line input on both USB ports at once (native USB CDC and the CP2102 UART).
#pragma once
#include <Arduino.h>

namespace sio {
void begin();
void out(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void raw(const char* line);        // one line, no formatting
bool readLine(String& line);       // non-blocking, true when a full line arrived
void setReliable(bool on);         // wait for every byte (screenshots)
}
