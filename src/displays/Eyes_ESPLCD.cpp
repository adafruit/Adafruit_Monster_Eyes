/**
 * @file Eyes_ESPLCD.cpp
 * @brief Display backend: ESP-IDF esp_lcd, SPI panel IO with async DMA.
 */

#include "Eyes_ESPLCD.h"

#if defined(ARDUINO_ARCH_ESP32)

#include <driver/spi_master.h>
#include <esp_heap_caps.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>

// GC9A01A vendor initialisation, transcribed from Adafruit_GC9A01A.cpp.
// Format: command, length, data... A length with 0x80 set means "then wait".
static const uint8_t gc9a01aInit[] = {
    0xEF, 0,    0xEB, 1,    0x14, 0xFE, 0,    0xEF, 0,    0xEB, 1,    0x14,
    0x84, 1,    0x40, 0x85, 1,    0xFF, 0x86, 1,    0xFF, 0x87, 1,    0xFF,
    0x88, 1,    0x0A, 0x89, 1,    0x21, 0x8A, 1,    0x00, 0x8B, 1,    0x80,
    0x8C, 1,    0x01, 0x8D, 1,    0x01, 0x8E, 1,    0xFF, 0x8F, 1,    0xFF,
    0xB6, 2,    0x00, 0x00, 0x36, 1,    0x48, // MADCTL: MX | BGR
    0x3A, 1,    0x05,                         // COLMOD: 16 bits per pixel
    0x90, 4,    0x08, 0x08, 0x08, 0x08, 0xBD, 1,    0x06, 0xBC, 1,    0x00,
    0xFF, 3,    0x60, 0x01, 0x04, 0xC3, 1,    0x13, // POWER2
    0xC4, 1,    0x13,                               // POWER3
    0xC9, 1,    0x22,                               // POWER4
    0xBE, 1,    0x11, 0xE1, 2,    0x10, 0x0E, 0xDF, 3,    0x21, 0x0C, 0x02,
    0xF0, 6,    0x45, 0x09, 0x08, 0x08, 0x26, 0x2A, // Gamma 1
    0xF1, 6,    0x43, 0x70, 0x72, 0x36, 0x37, 0x6F, // Gamma 2
    0xF2, 6,    0x45, 0x09, 0x08, 0x08, 0x26, 0x2A, // Gamma 3
    0xF3, 6,    0x43, 0x70, 0x72, 0x36, 0x37, 0x6F, // Gamma 4
    0xED, 2,    0x1B, 0x0B, 0xAE, 1,    0x77, 0xCD, 1,    0x63, 0xE8, 1,
    0x34, // Frame rate
    0x62, 12,   0x18, 0x0D, 0x71, 0xED, 0x70, 0x70, 0x18, 0x0F, 0x71, 0xEF,
    0x70, 0x70, 0x63, 12,   0x18, 0x11, 0x71, 0xF1, 0x70, 0x70, 0x18, 0x13,
    0x71, 0xF3, 0x70, 0x70, 0x64, 7,    0x28, 0x29, 0xF1, 0x01, 0xF1, 0x00,
    0x07, 0x66, 10,   0x3C, 0x00, 0xCD, 0x67, 0x45, 0x45, 0x10, 0x00, 0x00,
    0x00, 0x67, 10,   0x00, 0x3C, 0x00, 0x00, 0x00, 0x01, 0x54, 0x10, 0x32,
    0x98, 0x74, 7,    0x10, 0x85, 0x80, 0x00, 0x00, 0x4E, 0x00, 0x98, 2,
    0x3E, 0x07, 0x35, 0, // Tearing effect on
    0x21, 0,             // Inversion on
    0x11, 0x80,          // Sleep out, then wait
    0x29, 0x80,          // Display on, then wait
    0x00                 // End of list
};

// Outstanding DMA transfers per eye. File scope rather than members because
// the completion callback runs in interrupt context and only gets an eye index
// as its user context; the library is single-instance by design.
static volatile int s_pending[MONSTER_EYES_MAX_EYES] = {};

static bool IRAM_ATTR onColorDone(esp_lcd_panel_io_handle_t io,
                                  esp_lcd_panel_io_event_data_t *ev,
                                  void *ctx) {
  (void)io;
  (void)ev;
  const int e = (int)(intptr_t)ctx;
  if (s_pending[e] > 0)
    s_pending[e]--;
  return false;
}

Eyes_ESPLCD::Eyes_ESPLCD(EyesESPLCDDriver driver, int width, int height,
                         int sck, int mosi, int cs0, int dc0, int rst0, int cs1,
                         int dc1, int rst1)
    : Eyes_StripeDisplay((cs1 >= 0) ? 2 : 1, 16), _driver(driver), _sck(sck),
      _mosi(mosi), _pclkHz(40000000), _host(SPI2_HOST), _backlight(-1),
      _invert(true), _swapXY(false), _mirrorX(false), _mirrorY(false) {
  _panelW = width;
  _panelH = height;
  _cs[0] = cs0;
  _dc[0] = dc0;
  _rst[0] = rst0;
  _io[0] = NULL;
  _panel[0] = NULL;
#if MONSTER_EYES_MAX_EYES > 1
  _cs[1] = cs1;
  _dc[1] = (dc1 >= 0) ? dc1 : dc0;
  _rst[1] = (rst1 >= 0) ? rst1 : rst0;
  _io[1] = NULL;
  _panel[1] = NULL;
#else
  (void)cs1;
  (void)dc1;
  (void)rst1;
#endif
  // GC9A01A's stock orientation wants a horizontal mirror.
  if (driver == EYES_ESPLCD_GC9A01A)
    _mirrorX = false; // its vendor MADCTL already sets MX
}

Eyes_ESPLCD::~Eyes_ESPLCD() {}

void Eyes_ESPLCD::setOrientation(bool invert, bool swapXY, bool mirrorX,
                                 bool mirrorY) {
  _invert = invert;
  _swapXY = swapXY;
  _mirrorX = mirrorX;
  _mirrorY = mirrorY;
}

void Eyes_ESPLCD::panelSize(int *w, int *h) {
  *w = _panelW;
  *h = _panelH;
}

uint16_t *Eyes_ESPLCD::allocStripe(size_t bytes) {
  // MALLOC_CAP_DMA: handed straight to a DMA channel.
  return (uint16_t *)heap_caps_malloc(bytes, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
}

void Eyes_ESPLCD::freeStripe(uint16_t *p) { heap_caps_free(p); }

// Wait until at most `limit` transfers are outstanding for this eye. Bounded,
// so a misconfigured panel cannot lock the sketch up.
void Eyes_ESPLCD::drain(int eye, int limit) {
  uint32_t guard = 0;
  while ((s_pending[eye] > limit) && (++guard < 2000000)) { /* spin */
  }
  if (guard >= 2000000) {
    static bool warned = false;
    if (!warned) {
      warned = true;
      EYES_ERR("esp_lcd transfer stalled -- try the Eyes_TFT backend.\n");
    }
    s_pending[eye] = 0;
  }
}

void Eyes_ESPLCD::sendVendorInit(void *io) {
  const uint8_t *p = gc9a01aInit;
  uint8_t cmd;
  while ((cmd = *p++) != 0x00) {
    const uint8_t x = *p++;
    const uint8_t n = x & 0x7F;
    esp_lcd_panel_io_tx_param((esp_lcd_panel_io_handle_t)io, cmd, n ? p : NULL,
                              n);
    p += n;
    if (x & 0x80)
      delay(150);
  }
}

bool Eyes_ESPLCD::begin(void) {
  spi_bus_config_t bus = {};
  bus.sclk_io_num = _sck;
  bus.mosi_io_num = _mosi;
  bus.miso_io_num = -1;
  bus.quadwp_io_num = -1;
  bus.quadhd_io_num = -1;
  // Largest single transfer is a whole stripe, not a single column: with a
  // 16-column stripe on a 240-row panel that is 7680 bytes. Sizing this for
  // one column silently caps the stripe width, which is most of the point of
  // this backend.
  bus.max_transfer_sz = _maxStripeCols * _panelH * 2 + 64;

  if (spi_bus_initialize((spi_host_device_t)_host, &bus, SPI_DMA_CH_AUTO) !=
      ESP_OK) {
    EYES_ERR("spi_bus_initialize failed\n");
    return false;
  }

  resetPanels(_rst, _numEyes);

  for (int e = 0; e < _numEyes; e++) {
    esp_lcd_panel_io_spi_config_t io = {};
    io.cs_gpio_num = _cs[e];
    io.dc_gpio_num = _dc[e];
    io.spi_mode = 0;
    io.pclk_hz = _pclkHz;
    io.trans_queue_depth = 4;
    io.lcd_cmd_bits = 8;
    io.lcd_param_bits = 8;
    io.on_color_trans_done = onColorDone;
    io.user_ctx = (void *)(intptr_t)e;

    esp_lcd_panel_io_handle_t ioh = NULL;
    if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)_host, &io, &ioh) !=
        ESP_OK) {
      EYES_ERR("esp_lcd panel IO %d failed\n", e);
      return false;
    }
    _io[e] = ioh;

    esp_lcd_panel_dev_config_t pc = {};
    // Reset is driven once, above, so the panel object must not touch it.
    pc.reset_gpio_num = -1;
    pc.bits_per_pixel = 16;
    // GC9A01A wants BGR element order.
    const bool bgr = (_driver == EYES_ESPLCD_GC9A01A);
#if defined(ESP_IDF_VERSION) && defined(ESP_IDF_VERSION_VAL)
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 2, 0)
    pc.rgb_ele_order =
        bgr ? LCD_RGB_ELEMENT_ORDER_BGR : LCD_RGB_ELEMENT_ORDER_RGB;
#else
    pc.rgb_endian = bgr ? LCD_RGB_ENDIAN_BGR : LCD_RGB_ENDIAN_RGB;
#endif
#else
    pc.rgb_endian = bgr ? LCD_RGB_ENDIAN_BGR : LCD_RGB_ENDIAN_RGB;
#endif

    esp_lcd_panel_handle_t ph = NULL;
    if (esp_lcd_new_panel_st7789(ioh, &pc, &ph) != ESP_OK) {
      EYES_ERR("esp_lcd panel %d failed\n", e);
      return false;
    }
    _panel[e] = ph;

    if (_driver == EYES_ESPLCD_GC9A01A) {
      // The vendor sequence does everything panel_init() would: sleep out,
      // MADCTL, COLMOD, display on.
      sendVendorInit(ioh);
    } else {
      esp_lcd_panel_init(ph);
    }
    esp_lcd_panel_invert_color(ph, _invert);
    esp_lcd_panel_swap_xy(ph, _swapXY);
    esp_lcd_panel_mirror(ph, _mirrorX, _mirrorY);
    esp_lcd_panel_disp_on_off(ph, true);
    s_pending[e] = 0;
    EYES_DBG("  esp_lcd panel %d ready (CS=GPIO%d DC=GPIO%d)\n", e, _cs[e],
             _dc[e]);
  }

  if (_backlight >= 0) {
    pinMode(_backlight, OUTPUT);
    digitalWrite(_backlight, HIGH);
  }
  EYES_DBG("esp_lcd up: %d panel(s), %dx%d at %lu Hz\n", _numEyes, _panelW,
           _panelH, (unsigned long)_pclkHz);
  return true;
}

void Eyes_ESPLCD::fillPanel(int eye, uint16_t color) {
  if (!_scratch || (_stripePixels == 0)) {
    // Before setEyeSize() there is no buffer to fill from; harmless, since the
    // eye has not been drawn yet either.
    return;
  }
  const uint16_t v = __builtin_bswap16(color); // This backend is big-endian
  for (size_t i = 0; i < _stripePixels; i++)
    _scratch[i] = v;

  // Fill in bands that fit the stripe buffer. The buffer is sized for the EYE,
  // which can be smaller than the panel, so a full-height column would run off
  // the end of the allocation.
  const int bandRows = (int)(_stripePixels / (size_t)_stripeW);
  for (int y = 0; y < _panelH; y += bandRows) {
    const int rows = ((y + bandRows) <= _panelH) ? bandRows : (_panelH - y);
    for (int x = 0; x < _panelW; x += _stripeW) {
      const int cols = ((x + _stripeW) <= _panelW) ? _stripeW : (_panelW - x);
      drain(eye, 0);
      esp_lcd_panel_draw_bitmap((esp_lcd_panel_handle_t)_panel[eye], x, y,
                                x + cols, y + rows, _scratch);
      s_pending[eye]++;
    }
  }
  drain(eye, 0);
}

void Eyes_ESPLCD::clear(uint16_t color) {
  for (int e = 0; e < _numEyes; e++)
    fillPanel(e, color);
}

void Eyes_ESPLCD::flushStripe(int eye, int x0, int width, uint16_t *buf) {
  esp_lcd_panel_draw_bitmap((esp_lcd_panel_handle_t)_panel[eye], _originX + x0,
                            _originY, _originX + x0 + width,
                            _originY + _eyeSize, buf);
  s_pending[eye]++;
  // One transfer may stay in flight; the base class has already flipped to the
  // other buffer by the time the renderer writes again.
  drain(eye, 1);
}

void Eyes_ESPLCD::eyeEnd(int eye) { drain(eye, 0); }

void Eyes_ESPLCD::selfTest(void) {
  const uint16_t idColor[2] = {0xF800, 0x001F};
  const char *idName[2] = {"RED", "BLUE"};
  (void)idName;
  for (int e = 0; e < _numEyes; e++) {
    EYES_DBG("  self-test: panel %d -> %s   (CS=GPIO%d DC=GPIO%d)\n", e,
             idName[e], _cs[e], _dc[e]);
    fillPanel(e, idColor[e]);
  }
  EYES_DBG("  if a panel stays dark, its CS/DC GPIO numbers are wrong\n");
  delay(2500);
  clear(0x0000);
}

#endif // ARDUINO_ARCH_ESP32
