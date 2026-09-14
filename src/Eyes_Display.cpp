/**
 * @file Eyes_Display.cpp
 * @brief Shared backend behaviour: panel reset and stripe bookkeeping.
 */

#include "Eyes_Display.h"

Eyes_Display::Eyes_Display(uint8_t numEyes)
    : busyMicros(0), _stride(-1), _eyeSize(0), _numEyes(numEyes),
      _profile(false) {
  if (_numEyes < 1)
    _numEyes = 1;
  if (_numEyes > MONSTER_EYES_MAX_EYES)
    _numEyes = MONSTER_EYES_MAX_EYES;
}

Eyes_Display::~Eyes_Display() {}

void Eyes_Display::selfTest(void) {
  // Backends that can identify their panels override this.
  const uint16_t bars[3] = {0xF800, 0x001F, 0x0000};
  for (uint8_t i = 0; i < 3; i++) {
    clear(bars[i]);
    delay(500);
  }
}

void Eyes_Display::resetPanels(const int *rstPins, int count) {
  bool any = false;
  for (int i = 0; i < count; i++) {
    if (rstPins[i] < 0)
      continue;
    bool dup = false;
    for (int j = 0; j < i; j++)
      if (rstPins[j] == rstPins[i])
        dup = true;
    if (dup) {
      EYES_DBG("  panel %d shares RST GPIO%d\n", i, rstPins[i]);
      continue;
    }
    pinMode(rstPins[i], OUTPUT);
    digitalWrite(rstPins[i], HIGH);
    any = true;
  }
  if (!any)
    return;
  delay(10);
  for (int i = 0; i < count; i++)
    if (rstPins[i] >= 0)
      digitalWrite(rstPins[i], LOW);
  delay(20);
  for (int i = 0; i < count; i++)
    if (rstPins[i] >= 0)
      digitalWrite(rstPins[i], HIGH);
  delay(150); // Controllers want ~120 ms after reset before commands
  EYES_DBG("  panels reset\n");
}

// ===========================================================================
//  STRIPE BASE
// ===========================================================================

Eyes_StripeDisplay::Eyes_StripeDisplay(uint8_t numEyes, int maxStripeCols)
    : Eyes_Display(numEyes), _panelW(0), _panelH(0), _originX(0), _originY(0),
      _outScale(1), _stripeW(1), _stripeBase(0), _maxStripeCols(maxStripeCols),
      _buffers(1), _bufIdx(0), _scratch(NULL), _stripePixels(0) {}

Eyes_StripeDisplay::~Eyes_StripeDisplay() {
  if (_scratch)
    freeStripe(_scratch);
}

uint16_t *Eyes_StripeDisplay::allocStripe(size_t bytes) {
  return (uint16_t *)eyesMalloc(bytes);
}

void Eyes_StripeDisplay::freeStripe(uint16_t *p) { free(p); }

int Eyes_StripeDisplay::maxEyeSize(void) {
  if (_panelW <= 0)
    panelSize(&_panelW, &_panelH);
  const int w = (_panelW < _panelH) ? _panelW : _panelH;
  return w / _outScale;
}

bool Eyes_StripeDisplay::setEyeSize(int size) {
  if (_panelW <= 0)
    panelSize(&_panelW, &_panelH);

  _eyeSize = size;
  const int shown = size * _outScale;
  _originX = (_panelW - shown) / 2;
  _originY = (_panelH - shown) / 2;
  if (_originX < 0)
    _originX = 0;
  if (_originY < 0)
    _originY = 0;

  // A partial stripe would leave the buffer rows non-contiguous, so pick the
  // widest stripe that divides the eye exactly.
  _stripeW = 1;
  for (int w = _maxStripeCols; w >= 1; w--) {
    if ((size % w) == 0) {
      _stripeW = w;
      break;
    }
  }
  _stride = -_stripeW;

  if (_scratch) {
    freeStripe(_scratch);
    _scratch = NULL;
  }
  _buffers = buffersWanted();
  if (_buffers < 1)
    _buffers = 1;
  _stripePixels = (size_t)_stripeW * size;
  _scratch = allocStripe(_stripePixels * (size_t)_buffers * sizeof(uint16_t));
  _bufIdx = 0;
  _stripeBase = 0;
  if (!_scratch) {
    EYES_ERR("Stripe buffer allocation failed!\n");
    _stripePixels = 0;
    return false;
  }
  EYES_DBG("  stripes: %d columns, %d transfers per eye, %u bytes\n", _stripeW,
           size / _stripeW,
           (unsigned)(_stripePixels * _buffers * sizeof(uint16_t)));
  return true;
}

uint16_t *Eyes_StripeDisplay::column(int eye, int x) {
  (void)eye;
  if (!_scratch)
    return NULL;
  const int c = x % _stripeW; // Column within the stripe
  uint16_t *base = &_scratch[(size_t)_bufIdx * _stripePixels];
  if (c == 0)
    _stripeBase = x;
  // Bottom row of this column; the renderer walks up by one stripe width.
  return &base[(size_t)(_eyeSize - 1) * _stripeW + c];
}

void Eyes_StripeDisplay::columnDone(int eye, int x) {
  if (!_scratch)
    return;
  // Nothing leaves until the stripe is full.
  if (((x % _stripeW) != (_stripeW - 1)) && (x != _eyeSize - 1))
    return;

  uint32_t t0 = _profile ? micros() : 0;
  flushStripe(eye, _stripeBase, _stripeW,
              &_scratch[(size_t)_bufIdx * _stripePixels]);
  if (_buffers > 1)
    _bufIdx ^= 1;
  if (_profile)
    busyMicros += micros() - t0;
}
