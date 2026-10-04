// LovyanGFX display layer for ESP32-S3 + 3.5" ILI9488 (SPI, 480x320 landscape).
// Presents a TFT_eSPI-like API (drawString(str,x,y,fontNo), setTextDatum(TL_DATUM) ...)
// so the rest of the firmware stays unchanged.
#pragma once
#define LGFX_USE_V1
#include <Arduino.h>
#include <LovyanGFX.hpp>

// ---- pins (your wiring) ----
#define PIN_TFT_SCK  14
#define PIN_TFT_MOSI 13
#define PIN_TFT_DC   11
#define PIN_TFT_CS   10
#define PIN_TFT_RST  12
#define PIN_TFT_BL    8
#ifndef DISP_SPI_HZ
#define DISP_SPI_HZ 20000000        // 20 MHz is clean on your board; raise via build flag env if stable
#endif
#ifndef DISP_RGB_ORDER
#define DISP_RGB_ORDER 0
#endif
#ifndef DISP_INVERT
#define DISP_INVERT 0
#endif

class LGFX_Panel : public lgfx::LGFX_Device {
  lgfx::Panel_ILI9488 _panel;
  lgfx::Bus_SPI _bus;
  lgfx::Light_PWM _light;
public:
  LGFX_Panel() {
    { auto c = _bus.config();
      c.spi_host = SPI3_HOST;           // HSPI on ESP32-S3
      c.spi_mode = 0;
      c.freq_write = DISP_SPI_HZ;
      c.freq_read = 8000000;
      c.spi_3wire = false;
      c.use_lock = true;
      c.dma_channel = SPI_DMA_CH_AUTO;
      c.pin_sclk = PIN_TFT_SCK;
      c.pin_mosi = PIN_TFT_MOSI;
      c.pin_miso = -1;                  // SDO not needed
      c.pin_dc = PIN_TFT_DC;
      _bus.config(c);
      _panel.setBus(&_bus); }
    { auto c = _panel.config();
      c.pin_cs = PIN_TFT_CS;
      c.pin_rst = PIN_TFT_RST;
      c.pin_busy = -1;
      c.panel_width = 320;
      c.panel_height = 480;
      c.offset_x = 0; c.offset_y = 0;
      c.offset_rotation = 0;
      c.readable = false;
      c.invert = DISP_INVERT;
      c.rgb_order = DISP_RGB_ORDER;
      c.dlen_16bit = false;
      c.bus_shared = false;
      _panel.config(c); }
    { auto c = _light.config();
      c.pin_bl = PIN_TFT_BL;
      c.invert = false;
      c.freq = 5000;
      c.pwm_channel = 7;
      _light.config(c);
      _panel.setLight(&_light); }
    setPanel(&_panel);
  }
};

// colour names (only if the library did not already define them)
#ifndef TFT_BLACK
#define TFT_BLACK 0x0000
#endif
#ifndef TFT_NAVY
#define TFT_NAVY 0x000F
#endif
#ifndef TFT_DARKGREY
#define TFT_DARKGREY 0x7BEF
#endif
#ifndef TFT_LIGHTGREY
#define TFT_LIGHTGREY 0xD69A
#endif
#ifndef TFT_BLUE
#define TFT_BLUE 0x001F
#endif
#ifndef TFT_GREEN
#define TFT_GREEN 0x07E0
#endif
#ifndef TFT_CYAN
#define TFT_CYAN 0x07FF
#endif
#ifndef TFT_RED
#define TFT_RED 0xF800
#endif
#ifndef TFT_MAGENTA
#define TFT_MAGENTA 0xF81F
#endif
#ifndef TFT_YELLOW
#define TFT_YELLOW 0xFFE0
#endif
#ifndef TFT_WHITE
#define TFT_WHITE 0xFFFF
#endif
#ifndef TFT_ORANGE
#define TFT_ORANGE 0xFDA0
#endif

// TFT_eSPI-style datum numbers
#undef TL_DATUM
#undef TC_DATUM
#undef TR_DATUM
#undef ML_DATUM
#undef MC_DATUM
#undef MR_DATUM
#undef BL_DATUM
#undef BC_DATUM
#undef BR_DATUM
#define TL_DATUM 0
#define TC_DATUM 1
#define TR_DATUM 2
#define ML_DATUM 4
#define MC_DATUM 5
#define MR_DATUM 6
#define BL_DATUM 8
#define BC_DATUM 9
#define BR_DATUM 10

extern bool gDirty;     // set by Canvas drawing calls (sprite needs sending to the panel)

template <class B, bool TRACK>
class TftApi : public B {
  static const lgfx::IFont* fontFor(int f) {
    switch (f) {
      case 2: return &lgfx::fonts::Font2;
      case 4: return &lgfx::fonts::Font4;
      case 6: return &lgfx::fonts::Font6;
      case 7: return &lgfx::fonts::Font7;
      case 8: return &lgfx::fonts::Font8;
      default: return &lgfx::fonts::Font0;
    }
  }
  static inline void mark() { if (TRACK) gDirty = true; }
public:
  using B::B;
  // LovyanGFX datum values: top_left=0, top_center=1, top_right=2, middle_left=4, middle_center=5,
  // middle_right=6, bottom_left=16, bottom_center=17, bottom_right=18
  void setTextDatum(int d) {
    static const uint8_t map[11] = {0, 1, 2, 0, 4, 5, 6, 0, 16, 17, 18};
    B::setTextDatum((lgfx::textdatum::textdatum_t)map[d < 0 || d > 10 ? 0 : d]);
  }
  size_t drawString(const char* s, int32_t x, int32_t y, int font = 1) {
    mark();
    B::setFont(fontFor(font));
    return B::drawString(s, x, y);
  }
  size_t drawString(const String& s, int32_t x, int32_t y, int font = 1) { return drawString(s.c_str(), x, y, font); }
  void fillRect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t c) { mark(); B::fillRect(x, y, w, h, (uint16_t)c); }
  void drawRect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t c) { mark(); B::drawRect(x, y, w, h, (uint16_t)c); }
  void fillScreen(uint32_t c) { mark(); B::fillScreen((uint16_t)c); }
  void drawFastHLine(int32_t x, int32_t y, int32_t w, uint32_t c) { mark(); B::drawFastHLine(x, y, w, (uint16_t)c); }
  void drawFastVLine(int32_t x, int32_t y, int32_t h, uint32_t c) { mark(); B::drawFastVLine(x, y, h, (uint16_t)c); }
  void drawLine(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t c) { mark(); B::drawLine(x0, y0, x1, y1, (uint16_t)c); }
  void drawCircle(int32_t x, int32_t y, int32_t r, uint32_t c) { mark(); B::drawCircle(x, y, r, (uint16_t)c); }
  void fillCircle(int32_t x, int32_t y, int32_t r, uint32_t c) { mark(); B::fillCircle(x, y, r, (uint16_t)c); }
  void setTextColor(uint32_t fg, uint32_t bg) { B::setTextColor((uint16_t)fg, (uint16_t)bg); }
  void setTextColor(uint32_t fg) { B::setTextColor((uint16_t)fg); }
  // RGB565 pixels in native (little-endian) order, as produced by TJpg_Decoder / AnimatedGIF / the web panel.
  // (LovyanGFX treats a plain uint16_t* as byte-swapped, which gave the "psychedelic" colours.)
  void pushImage(int32_t x, int32_t y, int32_t w, int32_t h, const uint16_t* d) {
    B::pushImage(x, y, w, h, (const lgfx::rgb565_t*)d);
  }
  void setSwapBytes(bool) {}
};
class Display : public TftApi<LGFX_Panel, false> {};
class Canvas : public TftApi<lgfx::LGFX_Sprite, true> {
public:
  explicit Canvas(lgfx::LovyanGFX* parent) : TftApi<lgfx::LGFX_Sprite, true>(parent) {}
};
