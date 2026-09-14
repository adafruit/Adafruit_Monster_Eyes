/**
 * @file Eyes_DVI.h
 * @brief Display backend: PicoDVI framebuffer (RP2 only).
 *
 * One framebuffer holds every eye; with two they sit side by side, each centred
 * in half the screen, which caps eye size at width/2. A column of the eye runs
 * UP the screen, so the stride is one negative row and the renderer writes
 * straight into the framebuffer -- there is nothing to push afterwards.
 */

#ifndef _EYES_DVI_H_
#define _EYES_DVI_H_

#if defined(ARDUINO_ARCH_RP2040)

#include "../Eyes_Display.h"

class DVIGFX16; ///< Forward declared; the sketch includes PicoDVI.h itself

/**
 * @brief Renders into a PicoDVI 16-bit framebuffer.
 *
 * The sketch constructs the DVIGFX16 -- that is where video mode and carrier
 * board pinout belong -- but must NOT call its begin(). This backend does, at
 * the point in the startup sequence where the framebuffer allocation still has
 * a clean heap to work with. It is the single largest allocation in the sketch
 * (153,600 bytes at 320x240x16), so that ordering matters.
 */
class Eyes_DVI : public Eyes_Display {
public:
  /**
   * @param dvi     Framebuffer object from the sketch, not yet begun.
   * @param numEyes Eyes to fit side by side in the one framebuffer.
   */
  Eyes_DVI(DVIGFX16 *dvi, uint8_t numEyes = 2);

  bool begin(void) override;
  int maxEyeSize(void) override;
  bool setEyeSize(int size) override;
  void clear(uint16_t color) override;
  uint16_t *column(int eye, int x) override;
  void selfTest(void) override;

private:
  DVIGFX16 *_dvi;                      ///< Sketch's framebuffer object
  int _fbW;                            ///< Framebuffer width
  int _fbH;                            ///< Framebuffer height
  int _originX[MONSTER_EYES_MAX_EYES]; ///< Eye left edge per eye
  int _originY;                        ///< Eye top edge, shared
};

#endif // ARDUINO_ARCH_RP2040
#endif // _EYES_DVI_H_
