/**
 * @file Eyes_RGB666.h
 * @brief Display backend: parallel RGB666 panels via Arduino_GFX (ESP32 only).
 *
 * Aimed at the Adafruit Qualia ESP32-S3, whose round displays are far larger
 * than the eye needs to be rendered. The eye is rendered small and each pixel
 * replicated on the way out, which is what makes a 720x720 panel affordable:
 *
 *     eye 240   126 KB   internal SRAM
 *     eye 480   484 KB   PSRAM only
 *     eye 720  1076 KB   PSRAM only
 */

#ifndef _EYES_RGB666_H_
#define _EYES_RGB666_H_

#if defined(ARDUINO_ARCH_ESP32)

#include "../Eyes_Display.h"

class Arduino_RGB_Display; ///< Forward declared; sketch includes Arduino_GFX

/**
 * @brief Renders into an Arduino_GFX RGB666 framebuffer, scaling on output.
 *
 * The sketch constructs the whole Arduino_GFX stack -- I2C expander, RGB timing
 * generator with its porch values, and the display with its panel init
 * operations -- because those are properties of the panel you actually
 * attached, and there are more of them than any library table could keep up
 * with. Arduino_GFX ships the constants (TL021WVC02_init_operations and
 * friends), so the sketch just names them.
 *
 * Do NOT call the display's begin(); this backend does, in the right order.
 * The backlight is on the expander, so the sketch owns that too -- see the
 * Qualia_RGB666 example.
 */
class Eyes_RGB666 : public Eyes_StripeDisplay {
public:
  /**
   * @param gfx   Display object from the sketch, not yet begun.
   * @param scale Panel pixels per rendered pixel. 2 suits a 480 panel from a
   *              240 eye, 3 a 720 panel.
   */
  Eyes_RGB666(Arduino_RGB_Display *gfx, uint8_t scale = 2);
  ~Eyes_RGB666();

  bool begin(void) override;
  bool setEyeSize(int size) override;
  void clear(uint16_t color) override;
  void selfTest(void) override;

protected:
  void panelSize(int *w, int *h) override;
  void flushStripe(int eye, int x0, int width, uint16_t *buf) override;

private:
  Arduino_RGB_Display *_gfx; ///< Sketch's display object
  uint16_t *_blitbuf;        ///< One stripe expanded to panel pixels
  size_t _blitPixels;        ///< Capacity of _blitbuf
};

#endif // ARDUINO_ARCH_ESP32
#endif // _EYES_RGB666_H_
