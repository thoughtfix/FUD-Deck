// Board profile: ESP32-DIV V2 hardware (CiferTech's board; FUD-Deck is not related to its
// firmware). Pin map and measured constants, every value confirmed with tools/probe unless
// marked otherwise.
//
// One line per pin. On another board, give each part its own GPIO here; nothing else in the
// firmware names a pin number. V2's shared lines (marked below) are wiring, not firmware:
// a board that keeps them apart just lists different pins, and the boot checks adapt (POST
// finds nRF24 #3 usable when its CE is not tied to the IR driver).
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
// SD clock. 1 MHz, not 4: on this board the slot was hand-resoldered and the card is flaky
// at 4 MHz (first access after mount fails). Log writes are tiny, so the cost is nothing.
#define SD_HZ 1000000

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

// IR. V2: double-booked with nRF24 #3 (TX = its CE, RX = its CSN).
#define PIN_IR_TX 14
#define PIN_IR_RX 21

// Indicators
#define PIN_NEOPIXEL 1
#define NEOPIXEL_COUNT 4
#define PIN_BUZZER 2      // V2: shared with the battery divider below (speaker stage unfitted)

// Battery voltage divider (ADC). V2: IO2, the buzzer's line. Ratio measured on our unit.
#define PIN_VBAT_SENSE 2
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
