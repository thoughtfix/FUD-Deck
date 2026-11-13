#include "bus.h"
#include "pins.h"
#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace bus {

static SemaphoreHandle_t mtx = nullptr;
static bool nrf3 = false;
void setNrf3Usable(bool yes) { nrf3 = yes; }
void lock() { if (mtx) xSemaphoreTakeRecursive(mtx, portMAX_DELAY); }
void unlock() { if (mtx) xSemaphoreGiveRecursive(mtx); }

void sdReleaseQuick() {
  deselectAll();
  SPI.beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE0));
  SPI.transfer(0xFF);
  SPI.transfer(0xFF);
  SPI.endTransaction();
}

void deselectAll() {
  const uint8_t cs[] = {PIN_SD_CS, PIN_NRF1_CSN, PIN_NRF2_CSN, PIN_CC_CS};
  for (uint8_t p : cs) { pinMode(p, OUTPUT); digitalWrite(p, HIGH); }
  if (nrf3) { pinMode(PIN_NRF3_CSN, OUTPUT); digitalWrite(PIN_NRF3_CSN, HIGH); }
  else pinMode(PIN_NRF3_CSN, INPUT_PULLUP);   // the TSOP's line: listen, never drive
  const uint8_t ce[] = {PIN_NRF1_CE, PIN_NRF2_CE, PIN_NRF3_CE};
  for (uint8_t p : ce) { pinMode(p, OUTPUT); digitalWrite(p, LOW); }
}

// Measured on our unit: at the ESP32 default pad drive (2) the long shield/umbilical stub
// rings and SD writes fail at ANY clock (0/20 at 1 MHz). Drive 0, 1 or 3 all pass 60/60 up
// to 20 MHz. 1 = gentler edges, still plenty for the radios. Re-applied after every
// SPI.begin; it also survives a software reset, so a reboot alone does not restore it.
void setDrive() {
  gpio_set_drive_capability((gpio_num_t)PIN_SPI_SCK, GPIO_DRIVE_CAP_1);
  gpio_set_drive_capability((gpio_num_t)PIN_SPI_MOSI, GPIO_DRIVE_CAP_1);
}

void begin() {
  if (!mtx) mtx = xSemaphoreCreateRecursiveMutex();
  deselectAll();
  SPI.begin(PIN_SPI_SCK, PIN_SPI_MISO, PIN_SPI_MOSI, -1);
  setDrive();
}

void sdRelease() {
  deselectAll();
  // A failed SD.begin can leave the bus unusable, so start it fresh first.
  SPI.end();
  SPI.begin(PIN_SPI_SCK, PIN_SPI_MISO, PIN_SPI_MOSI, -1);
  setDrive();
  SPI.beginTransaction(SPISettings(400000, MSBFIRST, SPI_MODE0));
  for (int i = 0; i < 10; i++) SPI.transfer(0xFF);
  SPI.endTransaction();
}

bool nrf3CeWorks() {
  gpio_num_t g = (gpio_num_t)PIN_NRF3_CE;
  gpio_reset_pin(g);
  gpio_set_drive_capability(g, GPIO_DRIVE_CAP_0);  // ~5 mA: gentle on a clamped line
  gpio_set_direction(g, GPIO_MODE_INPUT_OUTPUT);
  gpio_set_level(g, 1);
  delayMicroseconds(100);
  int hi = gpio_get_level(g);
  gpio_set_level(g, 0);
  delayMicroseconds(50);
  int lo = gpio_get_level(g);
  pinMode(PIN_NRF3_CE, OUTPUT);
  digitalWrite(PIN_NRF3_CE, LOW);
  return hi == 1 && lo == 0;
}

}  // namespace bus
