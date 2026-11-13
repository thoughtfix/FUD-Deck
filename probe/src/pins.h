// ESP32-DIV V2 pin map, as the probe understands it.
// Source of truth for now: stock V2 config (shared.h #else branches, User_Setup v2.h)
// and the V2 schematics. The probe exists to confirm or correct this file.
#pragma once
#include <stdint.h>

// Display: ILI9341 240x320 on its own SPI host (HSPI / SPI3)
#define PIN_TFT_SCLK 36
#define PIN_TFT_MOSI 35
#define PIN_TFT_MISO 37
#define PIN_TFT_CS   17
#define PIN_TFT_DC   16
#define PIN_TFT_RST  0    // shared with the BOOT button / strapping pin
#define PIN_TFT_BL   7

// Touch: XPT2046, same bus as the display, polled (no IRQ in stock firmware)
#define PIN_TOUCH_CS 18

// Touch calibration, raw XPT2046 values at the screen edges, upright as held. Measured.
// Both axes run backwards: raw high at left/top.
#define TOUCH_RAW_X_LEFT   3617
#define TOUCH_RAW_X_RIGHT  306
#define TOUCH_RAW_Y_TOP    3637
#define TOUCH_RAW_Y_BOTTOM 209

// I2C: PCF8574 buttons, IP5306
#define PIN_SDA 8
#define PIN_SCL 9

// Shared radio/SD SPI bus (FSPI / SPI2)
#define PIN_SPI_SCK  12
#define PIN_SPI_MISO 13
#define PIN_SPI_MOSI 11

#define PIN_SD_CS 10
#define PIN_SD_CD 38

// nRF24 radios (CE, CSN)
#define PIN_NRF1_CE 15
#define PIN_NRF1_CSN 4
#define PIN_NRF2_CE 47
#define PIN_NRF2_CSN 48
#define PIN_NRF3_CE 14   // also IR TX
#define PIN_NRF3_CSN 21  // also IR RX

// CC1101
#define PIN_CC_CS   5
#define PIN_CC_GDO0 6
#define PIN_CC_GDO2 3    // strapping pin

// IR (double-booked with nRF24 #3)
#define PIN_IR_TX 14
#define PIN_IR_RX 21

// Indicators
#define PIN_NEOPIXEL 1
#define NEOPIXEL_COUNT 4
#define PIN_BUZZER 2

// Battery divider on IO2 (shared with the unfitted speaker stage). Measured on our unit.
#define VBAT_RATIO 1.47f

// PCF8574 bit map, as the board is held. Confirmed on our unit, matches stock V2.
#define PCF_BIT_UP     7
#define PCF_BIT_SELECT 6
#define PCF_BIT_DOWN   5
#define PCF_BIT_RIGHT  4
#define PCF_BIT_LEFT   3

// GPIOs no stock config claims. The probe only ever reads these (input + pull test).
// 39-42 are pad JTAG (unused, we debug over USB). 45 and 46 are strapping pins.
static const uint8_t FREE_PINS[] = {39, 40, 41, 42, 45, 46};
