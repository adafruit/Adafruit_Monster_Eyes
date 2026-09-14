/**
 * @file Eyes_ESPLCD.h
 * @brief Display backend: ESP-IDF esp_lcd, SPI panel IO with async DMA.
 *
 * The Adafruit_GFX path pushes pixels with the CPU. esp_lcd hands the buffer to
 * a DMA channel and returns, calling back when it lands, so a frame costs
 * max(render, transfer) rather than their sum. Same physical panels as
 * Eyes_TFT, different plumbing.
 *
 * This is the one backend with no panel object for the sketch to construct --
 * esp_lcd owns the panel itself -- so it takes pins directly and is passed to
 * Adafruit_Monster_Eyes as an Eyes_Display.
 */

#ifndef _EYES_ESPLCD_H_
#define _EYES_ESPLCD_H_

#if defined(ARDUINO_ARCH_ESP32)

#include "../Eyes_Display.h"

/** @brief Which panel controller esp_lcd should drive. */
enum EyesESPLCDDriver {
  EYES_ESPLCD_ST7789,  ///< ST7789, shipped in ESP-IDF core
  EYES_ESPLCD_GC9A01A, ///< GC9A01A; vendor init is sent by this backend
};

/**
 * @brief Drives one or two SPI panels through ESP-IDF's esp_lcd with DMA.
 *
 * ESP-IDF ships only a few panel drivers in core, ST7789 among them. GC9A01A is
 * not one, so this backend creates an ST7789 panel object -- the addressing
 * commands are identical (CASET 0x2A, RASET 0x2B, RAMWR 0x2C) -- and sends the
 * GC9A01A vendor power-on sequence itself.
 */
class Eyes_ESPLCD : public Eyes_StripeDisplay {
public:
  /**
   * @param driver Panel controller.
   * @param width  Panel width in pixels.
   * @param height Panel height in pixels.
   * @param sck    SPI clock GPIO.
   * @param mosi   SPI data-out GPIO.
   * @param cs0    Chip select for panel 0.
   * @param dc0    Data/command for panel 0.
   * @param rst0   Reset for panel 0, or -1 if tied to board reset.
   * @param cs1    Chip select for panel 1, or -1 for a single eye.
   * @param dc1    Data/command for panel 1; defaults to @p dc0 when shared.
   * @param rst1   Reset for panel 1; defaults to @p rst0 when shared.
   */
  Eyes_ESPLCD(EyesESPLCDDriver driver, int width, int height, int sck, int mosi,
              int cs0, int dc0, int rst0, int cs1 = -1, int dc1 = -1,
              int rst1 = -1);
  ~Eyes_ESPLCD();

  bool begin(void) override;
  void clear(uint16_t color) override;
  void eyeEnd(int eye) override;
  void selfTest(void) override;

  /** @brief esp_lcd wants byte-swapped pixels; the buffer goes to DMA
   *  verbatim. @return Always true. */
  bool bigEndian(void) const override { return true; }

  /**
   * @brief Pixel clock. Call before begin().
   * @param hz SPI clock in Hertz; 40 MHz is typical.
   */
  void setSPISpeed(uint32_t hz) { _pclkHz = hz; }

  /**
   * @brief SPI host to drive. Call before begin().
   * @param host ESP-IDF host number, e.g. SPI2_HOST.
   */
  void setSPIHost(int host) { _host = host; }

  /**
   * @brief Panel orientation and colour inversion. Call before begin().
   *
   * Most ST7789 and GC9A01A panels want inversion on. The GC9A01A's stock
   * MADCTL is MX | BGR, so it usually wants mirrorX as well.
   *
   * @param invert  Invert colours.
   * @param swapXY  Exchange rows and columns.
   * @param mirrorX Mirror horizontally.
   * @param mirrorY Mirror vertically.
   */
  void setOrientation(bool invert, bool swapXY = false, bool mirrorX = false,
                      bool mirrorY = false);

  /**
   * @brief Backlight enable pin. Call before begin().
   * @param pin GPIO driven high after init, or -1 if not switchable.
   */
  void setBacklightPin(int pin) { _backlight = pin; }

protected:
  void panelSize(int *w, int *h) override;
  void flushStripe(int eye, int x0, int width, uint16_t *buf) override;
  uint16_t *allocStripe(size_t bytes) override;
  void freeStripe(uint16_t *p) override;
  /** @brief Two, so the renderer fills one while DMA drains the other.
   *  @return 2. */
  int buffersWanted(void) override { return 2; }

private:
  void sendVendorInit(void *io);
  void drain(int eye, int limit);
  void fillPanel(int eye, uint16_t color);

  EyesESPLCDDriver _driver;               ///< Panel controller
  int _sck;                               ///< SPI clock GPIO
  int _mosi;                              ///< SPI data-out GPIO
  int _cs[MONSTER_EYES_MAX_EYES];         ///< Chip select per panel
  int _dc[MONSTER_EYES_MAX_EYES];         ///< Data/command per panel
  int _rst[MONSTER_EYES_MAX_EYES];        ///< Reset per panel
  void *_io[MONSTER_EYES_MAX_EYES];       ///< esp_lcd_panel_io_handle_t
  void *_panel[MONSTER_EYES_MAX_EYES];    ///< esp_lcd_panel_handle_t
  uint32_t _pclkHz;                       ///< Pixel clock
  int _host;                              ///< SPI host
  int _backlight;                         ///< Backlight GPIO, or -1
  bool _invert;                           ///< Invert colours
  bool _swapXY;                           ///< Exchange rows and columns
  bool _mirrorX;                          ///< Mirror horizontally
  bool _mirrorY;                          ///< Mirror vertically
};

#endif // ARDUINO_ARCH_ESP32
#endif // _EYES_ESPLCD_H_
