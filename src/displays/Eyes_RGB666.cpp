/**
 * @file Eyes_RGB666.cpp
 * @brief Display backend: parallel RGB666 panels via Arduino_GFX (ESP32 only).
 */

#include "Eyes_RGB666.h"

#if defined(ARDUINO_ARCH_ESP32) &&                                             \
    (CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32P4)

// Confined to this translation unit on purpose. Arduino_GFX defines bare colour
// names (BLACK, WHITE, RED...) that would otherwise collide with anything else
// a sketch includes; keeping it here means a Qualia sketch only meets them if
// it asks for them.
#include <Arduino_GFX_Library.h>
#include <esp_heap_caps.h>

Eyes_RGB666::Eyes_RGB666(Arduino_RGB_Display *gfx, uint8_t scale)
    : Eyes_StripeDisplay(1, 8), _gfx(gfx), _blitbuf(NULL), _blitPixels(0) {
  _outScale = (scale < 1) ? 1 : scale;
}

Eyes_RGB666::~Eyes_RGB666() {
  if (_blitbuf)
    heap_caps_free(_blitbuf);
}

void Eyes_RGB666::panelSize(int *w, int *h) {
  *w = _gfx ? _gfx->width() : 0;
  *h = _gfx ? _gfx->height() : 0;
}

bool Eyes_RGB666::begin(void) {
  if (!_gfx) {
    EYES_ERR("Eyes_RGB666: no display given\n");
    return false;
  }
  if (!_gfx->begin()) {
    EYES_ERR("Arduino_GFX begin() failed -- is PSRAM enabled?\n");
    return false;
  }
  _gfx->fillScreen(0);
  panelSize(&_panelW, &_panelH);
  EYES_DBG("RGB666 up: %dx%d, framebuffer %u bytes in PSRAM\n", _panelW,
           _panelH, (unsigned)((size_t)_panelW * _panelH * 2));
  return true;
}

bool Eyes_RGB666::setEyeSize(int size) {
  if (!Eyes_StripeDisplay::setEyeSize(size))
    return false;

  if (_blitbuf) {
    heap_caps_free(_blitbuf);
    _blitbuf = NULL;
  }
  // INTERNAL RAM, NOT PSRAM. The framebuffer has to live in PSRAM, and the
  // LCD peripheral's DMA is reading it continuously to feed the panel. Every
  // byte we put on that same bus competes with the scanout, and when the line
  // buffer underruns the panel shifts the whole image sideways for a frame.
  //
  // A PSRAM blit buffer costs three PSRAM accesses per output pixel -- write
  // it here, then draw16bitRGBBitmap() reads it back and writes the
  // framebuffer -- against one for an internal buffer. It is only about 15 KB
  // at typical settings, so there is no reason to pay that.
  const int S = _outScale;
  _blitPixels = (size_t)_stripeW * S * size * S;
  _blitbuf = (uint16_t *)heap_caps_malloc(
      _blitPixels * sizeof(uint16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!_blitbuf) {
    EYES_DBG("  blit buffer fell back to PSRAM (%u bytes); expect the image "
             "to shift sideways under load\n",
             (unsigned)(_blitPixels * sizeof(uint16_t)));
    _blitbuf = (uint16_t *)heap_caps_malloc(
        _blitPixels * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  }
  if (!_blitbuf) {
    EYES_ERR("Blit buffer allocation failed!\n");
    _blitPixels = 0;
    return false;
  }
  EYES_DBG("  rendering %dx%d, showing %dx%d (scale %d)\n", size, size,
           size * S, size * S, S);
  return true;
}

void Eyes_RGB666::clear(uint16_t color) {
  if (_gfx)
    _gfx->fillScreen(color);
}

void Eyes_RGB666::flushStripe(int eye, int x0, int width, uint16_t *buf) {
  (void)eye;
  if (!_blitbuf)
    return;
  const int S = _outScale;
  const int outW = width * S;

  for (int r = 0; r < _eyeSize; r++) {
    const uint16_t *src = &buf[(size_t)r * width];
    for (int sy = 0; sy < S; sy++) {
      uint16_t *dst = &_blitbuf[(size_t)(r * S + sy) * outW];
      for (int c = 0; c < width; c++) {
        const uint16_t v = src[c];
        for (int sx = 0; sx < S; sx++)
          *dst++ = v;
      }
    }
  }
  _gfx->draw16bitRGBBitmap(_originX + x0 * S, _originY, _blitbuf, outW,
                           _eyeSize * S);
}

void Eyes_RGB666::selfTest(void) {
  const uint16_t bars[4] = {0xF800, 0x07E0, 0x001F, 0xFFFF};
  for (uint8_t i = 0; i < 4; i++) {
    _gfx->fillScreen(bars[i]);
    delay(600);
  }
  _gfx->fillScreen(0);
  _gfx->fillRect(0, 0, _panelW / 2, _panelH / 2, 0xFFE0);
  EYES_DBG("  yellow block should sit TOP-LEFT\n");
  delay(1200);
}

#endif // ESP32-S3 / ESP32-P4
