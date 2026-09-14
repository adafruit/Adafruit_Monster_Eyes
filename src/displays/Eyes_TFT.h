/**
 * @file Eyes_TFT.h
 * @brief Display backend: SPI TFT panels driven by Adafruit_GFX.
 *
 * Portable across RP2 and ESP32, and works with any Adafruit_SPITFT subclass
 * -- ST7789, GC9A01A, ILI9341, anything else with the same base. The sketch
 * constructs and initialises the panel; this backend only pushes pixels.
 *
 * On RP2 it can additionally batch pixels straight into the SPI hardware and
 * drive them with DMA, so transfers overlap the next column's render. See
 * setFastSPI().
 */

#ifndef _EYES_TFT_H_
#define _EYES_TFT_H_

#include "../Eyes_Display.h"

class Adafruit_SPITFT; ///< Forward declared; the sketch includes its own panel

/**
 * @brief Drives one or two SPI TFT panels through Adafruit_GFX.
 *
 * The sketch owns the panel objects, which means panel type, rotation, SPI
 * speed and bus are all its business -- this class asks the panel how big it
 * is and writes to it.
 *
 * IMPORTANT: initialise the panels yourself before Adafruit_Monster_Eyes
 * ::begin(), and pulse any shared reset line first:
 *
 *     Adafruit_Monster_Eyes::resetPanels(TFT_RST);
 *     panel0.init(240, 240);
 *     panel1.init(240, 240);
 *
 * Adafruit_GFX has no common init call across drivers -- ST7789 wants
 * init(w, h) while GC9A01A and ILI9341 want begin() -- so this is the one
 * place the sketch has to do the work. Construct the panels with a reset pin
 * of -1 so no driver pulses a shared reset line during its own init; with a
 * shared line, the second panel's init would otherwise knock the first back to
 * its power-on state, and the symptom is a panel that flashes an image at boot
 * and then stays dark while still receiving pixels.
 */
class Eyes_TFT : public Eyes_StripeDisplay {
public:
  /**
   * @param panel0 First panel; becomes eye 0. Must already be initialised.
   * @param panel1 Second panel, or NULL for a single eye.
   */
  Eyes_TFT(Adafruit_SPITFT *panel0, Adafruit_SPITFT *panel1 = NULL);
  ~Eyes_TFT();

  bool begin(void) override;
  void clear(uint16_t color) override;
  void eyeBegin(int eye) override;
  void eyeEnd(int eye) override;
  void selfTest(void) override;

  /**
   * @brief RP2 only: batch pixels into the SPI hardware and DMA them out.
   *
   * Worth a large chunk of the frame rate, because the transfer then overlaps
   * the next column's render instead of following it. Needs the SCK pin so it
   * can work out which SPI block the panel is on; pass the same pin you gave
   * SPI.setSCK(). No effect on other chips.
   *
   * Call before Adafruit_Monster_Eyes::begin().
   *
   * @param sck0 SCK GPIO for panel 0.
   * @param sck1 SCK GPIO for panel 1, or -1 to reuse @p sck0.
   * @param useDma false to batch without DMA, which is slower but simpler to
   *               debug.
   */
  void setFastSPI(int sck0, int sck1 = -1, bool useDma = true);

protected:
  void panelSize(int *w, int *h) override;
  void flushStripe(int eye, int x0, int width, uint16_t *buf) override;
  int buffersWanted(void) override;

private:
  Adafruit_SPITFT *_panel[MONSTER_EYES_MAX_EYES]; ///< Sketch's panel objects
  int _sck[MONSTER_EYES_MAX_EYES];                ///< SCK pin per panel
  bool _fast;                                     ///< Batched SPI requested
  bool _dma;                                      ///< DMA requested
#if defined(ARDUINO_ARCH_RP2040)
  void dmaSettle(int eye);
  void *_spiInst[MONSTER_EYES_MAX_EYES];  ///< spi_inst_t*, kept opaque here
  int _dmaCh[MONSTER_EYES_MAX_EYES];      ///< DMA channel, or -1
  bool _dmaActive[MONSTER_EYES_MAX_EYES]; ///< A transfer is in flight
#endif
};

#endif // _EYES_TFT_H_
