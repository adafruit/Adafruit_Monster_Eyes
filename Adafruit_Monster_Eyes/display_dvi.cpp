/**
 * @file display_dvi.cpp
 * @brief Display backend: PicoDVI framebuffer (RP2 only).
 *
 * One framebuffer holds every eye; with two they sit side by side, each
 * centred in half the screen, which caps eye size at width/2. A column of the
 * eye runs UP the screen, so the stride is one negative row.
 */

#include "eye.h"

#if EYE_DISPLAY == EYE_DISPLAY_DVI

#  include <Arduino.h>
#  include <PicoDVI.h>

static DVIGFX16 dvi(DVI_RESOLUTION, DVI_PIN_CONFIG);

static int fbW = 0, fbH = 0;
static int originX[NUM_EYES], originY = 0, eyeSize = 0;

int displayColumnStride = 0;
volatile uint32_t displayBusyMicros = 0; // Always 0: nothing to push

bool displayBegin(void) {
  // This allocates the framebuffer -- 153,600 bytes at 320x240x16. It is the
  // single largest allocation in the sketch, so it must happen before the
  // polar maps
  if (!dvi.begin()) return false;
  fbW = dvi.width();
  fbH = dvi.height();
  displayColumnStride = -fbW;
  DBG("DVI up: %dx%d, %d eye(s)\n", fbW, fbH, NUM_EYES);
  return true;
}

int displayMaxEyeSize(void) {
  int w = fbW / NUM_EYES; // Each eye gets an equal horizontal slice
  return (w < fbH) ? w : fbH;
}

void displaySetEyeSize(int size) {
  eyeSize = size;
  originY = (fbH - size) / 2;
  if (originY < 0) originY = 0;
  const int slice = fbW / NUM_EYES;
  for (int e = 0; e < NUM_EYES; e++) {
    originX[e] = e * slice + (slice - size) / 2;
    if (originX[e] < 0) originX[e] = 0;
  }
}

void displayClear(uint16_t color) {
  dvi.fillScreen(color);
}

void displayFrameBegin(void) {}
void displayEyeBegin(int eye) {
  (void)eye;
}

uint16_t *displayColumn(int eye, int x) {
  // Start at the TOP of the column; the renderer walks up-screen
  return &dvi.getBuffer()[(originY + eyeSize - 1) * fbW + originX[eye] + x];
}

void displayColumnDone(int eye, int x) {
  (void)eye;
  (void)x;
}

void displayEyeEnd(int eye) {
  (void)eye;
}
void displayFrameEnd(void) {}

void displaySelfTest(void) {
  const uint16_t bars[4] = {0xF800, 0x07E0, 0x001F, 0xFFFF};
  for (uint8_t i = 0; i < 4; i++) {
    dvi.fillScreen(bars[i]);
    delay(600);
  }
  dvi.fillScreen(0);
  dvi.fillRect(0, 0, fbW / 2, fbH / 2, 0xFFE0);
  delay(1200);
}

#endif // EYE_DISPLAY == EYE_DISPLAY_DVI
