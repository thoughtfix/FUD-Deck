#include "display.h"

FudDisplay tft;

FudDisplay::FudDisplay() {
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
    // Raw values only. input.cpp maps them with our measured calibration.
    auto c = _touch.config();
    c.x_min = 0;
    c.x_max = 4095;
    c.y_min = 0;
    c.y_max = 4095;
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

namespace display {
void begin() {
  tft.init();
  tft.setRotation(ROTATION);
  tft.setBrightness(255);
  tft.fillScreen(TFT_BLACK);
}
}  // namespace display
