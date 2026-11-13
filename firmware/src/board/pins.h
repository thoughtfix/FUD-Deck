// Pin map: picks the board profile chosen by the build environment (platformio.ini).
// To add a board: copy boards/esp32div_v2.h, change the pins, add an env with its define.
#pragma once

#if defined(FUD_BOARD_ESP32DIV_V2)
#include "boards/esp32div_v2.h"
#else
#error "No board selected: build with an env from platformio.ini (for example -e div2)."
#endif
