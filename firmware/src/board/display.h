// ILI9341 240x320 + XPT2046 touch on their own SPI host (HSPI). Portrait, as held.
#pragma once
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include "pins.h"

class FudDisplay : public lgfx::LGFX_Device {
  lgfx::Panel_ILI9341 _panel;
  lgfx::Bus_SPI _bus;
  lgfx::Light_PWM _light;
  lgfx::Touch_XPT2046 _touch;

 public:
  FudDisplay();
};

extern FudDisplay tft;

namespace display {
void begin();
// Upright as the board is held (D-pad below the screen). TFT_eSPI's "2" is LovyanGFX's 0.
constexpr uint8_t ROTATION = 0;
}
