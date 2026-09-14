/**
 * @file Eyes_DVI.cpp
 * @brief Display backend: PicoDVI framebuffer (RP2 only).
 */

#include "Eyes_DVI.h"

#if defined(ARDUINO_ARCH_RP2040)

#include <PicoDVI.h>

Eyes_DVI::Eyes_DVI(DVIGFX16 *dvi, uint8_t numEyes)
    : Eyes_Display(numEyes), _dvi(dvi), _fbW(0), _fbH(0), _originY(0) {
  for (int e = 0; e < MONSTER_EYES_MAX_EYES; e++)
    _originX[e] = 0;
}

bool Eyes_DVI::begin(void) {
  if (!_dvi) {
    EYES_ERR("Eyes_DVI: no DVIGFX16 given\n");
    return false;
  }
  // This allocates the framebuffer, the single largest allocation in the
  // sketch, which is why it happens before the polar maps.
  if (!_dvi->begin()) {
    EYES_ERR("PicoDVI begin() failed -- wrong pin config, or out of RAM?\n");
    return false;
  }
  _fbW = _dvi->width();
  _fbH = _dvi->height();
  _stride = -_fbW; // A column runs UP the screen
  EYES_DBG("DVI up: %dx%d, %d eye(s)\n", _fbW, _fbH, _numEyes);
  return true;
}

int Eyes_DVI::maxEyeSize(void) {
  const int w = _fbW / _numEyes; // Each eye gets an equal horizontal slice
  return (w < _fbH) ? w : _fbH;
}

bool Eyes_DVI::setEyeSize(int size) {
  _eyeSize = size;
  _originY = (_fbH - size) / 2;
  if (_originY < 0)
    _originY = 0;
  const int slice = _fbW / _numEyes;
  for (int e = 0; e < _numEyes; e++) {
    _originX[e] = e * slice + (slice - size) / 2;
    if (_originX[e] < 0)
      _originX[e] = 0;
  }
  return true; // Nothing to allocate; the framebuffer already exists
}

void Eyes_DVI::clear(uint16_t color) { _dvi->fillScreen(color); }

uint16_t *Eyes_DVI::column(int eye, int x) {
  // Start at the TOP of the column; the renderer walks up-screen.
  return &_dvi->getBuffer()[(_originY + _eyeSize - 1) * _fbW + _originX[eye] +
                            x];
}

void Eyes_DVI::selfTest(void) {
  const uint16_t bars[4] = {0xF800, 0x07E0, 0x001F, 0xFFFF};
  for (uint8_t i = 0; i < 4; i++) {
    _dvi->fillScreen(bars[i]);
    delay(600);
  }
  _dvi->fillScreen(0);
  _dvi->fillRect(0, 0, _fbW / 2, _fbH / 2, 0xFFE0);
  EYES_DBG("  yellow block should sit TOP-LEFT\n");
  delay(1200);
}

#endif // ARDUINO_ARCH_RP2040
