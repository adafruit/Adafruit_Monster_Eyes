/**
 * @file Eyes_Display.h
 * @brief The display backend contract, and a base class for panels that take
 *        pixels a stripe at a time.
 *
 * Adding support for a new display means subclassing one of these two. See
 * extras/PORTING.md for a walkthrough.
 *
 * THE COLUMN CONTRACT, once per frame:
 *
 *     frameBegin();
 *     for (e = 0; e < eyeCount(); e++) {
 *       eyeBegin(e);
 *       for (x = 0; x < eyeSize(); x++) {
 *         uint16_t *p = column(e, x);
 *         // write exactly eyeSize() pixels, advancing p by columnStride()
 *         columnDone(e, x);
 *       }
 *       eyeEnd(e);
 *     }
 *     frameEnd();
 *
 * The stride lets each backend choose its own memory layout AND direction.
 * The renderer's +Y is up, so it produces a column bottom-first while a panel
 * wants it top-first. Rather than reverse it in a second pass, column() hands
 * back a pointer near the END of a buffer together with a NEGATIVE stride; the
 * renderer then fills it in exactly the order the panel wants. The framebuffer
 * backend does the same thing with a negative row stride. Both are verified
 * pixel-identical.
 */

#ifndef _EYES_DISPLAY_H_
#define _EYES_DISPLAY_H_

#include "Eyes_Platform.h"

/**
 * @brief Abstract display backend.
 *
 * The sketch constructs a concrete backend (or hands a panel object to
 * Adafruit_Monster_Eyes, which constructs one for it) and never calls these
 * methods directly. Adafruit_Monster_Eyes::begin() drives the whole sequence,
 * because the order is load-bearing -- see begin() below.
 */
class Eyes_Display {
public:
  Eyes_Display(uint8_t numEyes = 1);
  virtual ~Eyes_Display();

  /**
   * @brief Bring up the display surface.
   *
   * Called by Adafruit_Monster_Eyes::begin() after storage is mounted but
   * before the polar maps are built, because on a framebuffer backend this is
   * the single largest allocation in the sketch and it must not have to fight
   * fragmentation.
   *
   * A backend handed a panel object by the sketch must call that object's own
   * begin() here, NOT leave it to the sketch, or the ordering breaks.
   *
   * @return true on success.
   */
  virtual bool begin(void) = 0;

  /**
   * @brief Largest square one eye can occupy.
   *
   * With two eyes sharing a single framebuffer this is half the width, so a
   * config asking for more is clamped rather than overlapping its neighbour.
   *
   * @return Maximum eye width and height in pixels.
   */
  virtual int maxEyeSize(void) = 0;

  /**
   * @brief Fix the eye size and allocate any per-column state.
   *
   * Called before textures claim the heap: this is where stripe buffers are
   * allocated, and if they were requested afterwards the texture loader could
   * starve them, leaving a running frame counter and a blank screen.
   *
   * @param size Eye width and height in pixels.
   * @return true if the backend has the memory for it.
   */
  virtual bool setEyeSize(int size) = 0;

  /**
   * @brief Fill every panel with a solid colour.
   * @param color Native-endian RGB565; the backend converts if it needs to.
   */
  virtual void clear(uint16_t color) = 0;

  /** @brief Start a frame. */
  virtual void frameBegin(void) {}

  /**
   * @brief Start one eye; opens the bus transaction on SPI backends.
   * @param eye Eye index.
   */
  virtual void eyeBegin(int eye) { (void)eye; }

  /**
   * @brief Where to write column @p x of eye @p eye.
   * @param eye Eye index.
   * @param x   Column index, 0 to eyeSize()-1.
   * @return Buffer to write into, or NULL if no buffer could be allocated.
   */
  virtual uint16_t *column(int eye, int x) = 0;

  /**
   * @brief Hand a finished column back; the backend may send it or batch it.
   * @param eye Eye index.
   * @param x   Column index.
   */
  virtual void columnDone(int eye, int x) {
    (void)eye;
    (void)x;
  }

  /**
   * @brief Finish one eye, flushing anything still in flight.
   * @param eye Eye index.
   */
  virtual void eyeEnd(int eye) { (void)eye; }

  /** @brief Finish the frame. */
  virtual void frameEnd(void) {}

  /**
   * @brief Identify each panel with a solid fill, using the driver's own fill.
   *
   * Answers three questions at once: is each panel responding, which physical
   * display is which index, and are the chip selects independent.
   */
  virtual void selfTest(void);

  /**
   * @brief Does this backend want byte-swapped pixels?
   *
   * True where the buffer is handed to DMA verbatim. Textures are swapped once
   * at load and colours once per frame, so the render loop never pays for it.
   *
   * @return true for big-endian output.
   */
  virtual bool bigEndian(void) const { return false; }

  /** @brief Eyes this backend can show. @return 1 or 2. */
  uint8_t eyeCount(void) const { return _numEyes; }

  /** @brief Current eye size in pixels. @return Size, or 0 before setEyeSize().
   */
  int eyeSize(void) const { return _eyeSize; }

  /** @brief Pixels to advance between successive rows of a column.
   *  @return Signed stride. */
  int columnStride(void) const { return _stride; }

  /**
   * @brief Microseconds spent pushing pixels since this was last cleared.
   *
   * Zero on a framebuffer backend, where a column is already in the buffer and
   * there is nothing to push.
   */
  volatile uint32_t busyMicros;

  /**
   * @brief Account transfer time in busyMicros.
   * @param on true to time each flush.
   */
  void setProfile(bool on) { _profile = on; }

  /**
   * @brief Pulse every distinct reset pin once, before any panel is
   *        initialised.
   *
   * Panels should be constructed WITHOUT a reset pin so that no driver pulses
   * the line itself. With a SHARED reset, the second panel's init would
   * otherwise knock the first back to its power-on state -- the symptom being
   * a panel that flashes an image at boot and then stays dark while still
   * receiving pixels.
   *
   * Duplicate pin numbers are pulsed only once. Safe to call with all -1.
   *
   * @param rstPins Reset GPIO per eye; entries below zero are skipped.
   * @param count   Number of entries in @p rstPins.
   */
  static void resetPanels(const int *rstPins, int count);

protected:
  int _stride;      ///< Row-to-row step handed to the renderer
  int _eyeSize;     ///< Eye size in pixels, once known
  uint8_t _numEyes; ///< Eyes this backend drives
  bool _profile;    ///< Accumulate busyMicros
};

/**
 * @brief Base for panels that are fed a stripe of columns at a time.
 *
 * Every SPI-ish backend needs the same bookkeeping: pick a stripe width that
 * divides the eye size, allocate stripe buffers, hand out column pointers with
 * a negative stride, and flush when a stripe fills. That is all here, so a
 * subclass implements only what is genuinely panel-specific: how big the panel
 * is, and how to get one stripe onto it.
 *
 * Batching matters because each address window costs a fixed command sequence
 * regardless of size, so wider stripes are close to free frame rate. The cost
 * is memory: stripeWidth * eyeSize * 2 bytes per buffer.
 */
class Eyes_StripeDisplay : public Eyes_Display {
public:
  /**
   * @param numEyes      Panels this backend drives.
   * @param maxStripeCols Widest stripe to consider; the largest divisor of the
   *                      eye size at or below this wins.
   */
  Eyes_StripeDisplay(uint8_t numEyes, int maxStripeCols = 16);
  virtual ~Eyes_StripeDisplay();

  int maxEyeSize(void) override;
  bool setEyeSize(int size) override;
  uint16_t *column(int eye, int x) override;
  void columnDone(int eye, int x) override;

  /**
   * @brief Change the stripe width cap. Call before begin().
   * @param cols Columns per transfer.
   */
  void setStripeColumns(int cols) { _maxStripeCols = cols; }

protected:
  /**
   * @brief Panel dimensions in physical pixels.
   * @param w Receives width.
   * @param h Receives height.
   */
  virtual void panelSize(int *w, int *h) = 0;

  /**
   * @brief Get one finished stripe onto the panel.
   *
   * The buffer holds @p width columns of eyeSize() pixels, row-major, top row
   * first, so it can go straight out. Coordinates are in RENDERED pixels; add
   * originX()/originY() and multiply by outputScale() to reach panel pixels.
   *
   * @param eye   Eye index.
   * @param x0    First rendered column in the stripe.
   * @param width Columns in the stripe.
   * @param buf   Pixels to send.
   */
  virtual void flushStripe(int eye, int x0, int width, uint16_t *buf) = 0;

  /**
   * @brief Allocate the stripe buffers.
   *
   * Overridden where the memory has to come from somewhere particular -- a
   * DMA-capable pool, for instance.
   *
   * @param bytes Bytes needed.
   * @return Pointer, or NULL.
   */
  virtual uint16_t *allocStripe(size_t bytes);

  /**
   * @brief Release what allocStripe() returned.
   * @param p Buffer to free.
   */
  virtual void freeStripe(uint16_t *p);

  /**
   * @brief How many stripe buffers to allocate.
   *
   * Two lets the renderer fill one while the other is still in flight, which
   * is only useful where the transfer is asynchronous.
   *
   * @return 1 or 2.
   */
  virtual int buffersWanted(void) { return 1; }

  /** @brief Left edge of the eye in panel pixels. @return X origin. */
  int originX(void) const { return _originX; }
  /** @brief Top edge of the eye in panel pixels. @return Y origin. */
  int originY(void) const { return _originY; }
  /** @brief Panel pixels per rendered pixel. @return Scale factor. */
  int outputScale(void) const { return _outScale; }
  /** @brief Columns in a full stripe. @return Stripe width. */
  int stripeWidth(void) const { return _stripeW; }
  /** @brief Pixels one stripe buffer can hold. @return Capacity. */
  size_t stripeCapacity(void) const { return _stripePixels; }

  int _panelW;          ///< Panel width in pixels
  int _panelH;          ///< Panel height in pixels
  int _originX;         ///< Eye left edge in panel pixels
  int _originY;         ///< Eye top edge in panel pixels
  int _outScale;        ///< Panel pixels per rendered pixel; RGB666 uses > 1
  int _stripeW;         ///< Columns per transfer
  int _stripeBase;      ///< First column of the stripe being filled
  int _maxStripeCols;   ///< Cap on _stripeW
  int _buffers;         ///< Stripe buffers allocated
  uint8_t _bufIdx;      ///< Buffer currently being filled
  uint16_t *_scratch;   ///< _buffers * _stripePixels pixels
  size_t _stripePixels; ///< Pixels in one stripe buffer
};

#endif // _EYES_DISPLAY_H_