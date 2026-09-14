/**
 * @file Adafruit_Monster_Eyes.h
 * @brief Animated monster eyes for RP2040/RP2350 and ESP32.
 *
 * Ported from Adafruit's M4_Eyes by Phillip Burgess. MIT licensed.
 *
 * This is the only header a sketch needs to include: it pulls in every display
 * backend available for the chip you are compiling for.
 *
 * USAGE
 *
 *     #include <Adafruit_GC9A01A.h>
 *     #include <Adafruit_Monster_Eyes.h>
 *
 *     Adafruit_GC9A01A panel0(&SPI, TFT_DC, TFT_CS, -1);
 *     Adafruit_Monster_Eyes eyes(&panel0);
 *
 *     void setup() {
 *       SPI.begin();
 *       Adafruit_Monster_Eyes::resetPanels(TFT_RST);
 *       panel0.begin();
 *       eyes.begin();
 *     }
 *
 *     void loop() { eyes.animate(); }
 *
 * WHERE SETTINGS COME FROM
 *
 * Three layers, each beating the one above:
 *   1. built-in defaults, applied by the constructor
 *   2. setters called BEFORE begin()
 *   3. config.eye on the CIRCUITPY drive, read by begin()
 *
 * So drag-and-drop stays authoritative: whatever is on the drive wins over
 * what the sketch compiled in. Setters called AFTER begin() are live and win
 * over everything -- that is where sensor code belongs.
 */

#ifndef _ADAFRUIT_MONSTER_EYES_H_
#define _ADAFRUIT_MONSTER_EYES_H_

#include "Eyes_Display.h"
#include "Eyes_Platform.h"
#include "displays/Eyes_DVI.h"
#include "displays/Eyes_ESPLCD.h"
#include "displays/Eyes_RGB666.h"
#include "displays/Eyes_TFT.h"

#define EYES_PATH_MAX 64 ///< Longest asset path accepted from a config file

/** @brief Everything a config.eye file can change about the eye. */
struct EyesSettings {
  int displaySize;     ///< Eye width and height in pixels; 0 fills the display
  int eyeRadius;       ///< Eyeball radius in screen pixels; 0 derives it
  int irisRadius;      ///< Iris radius in screen pixels; 0 derives it
  int slitPupilRadius; ///< Slit pupil radius; 0 round, -1 derives it
  float coverage;      ///< Effective, possibly raised by finalize()
  float coverageRequested;   ///< What the sketch or config actually asked for
  uint16_t pupilColor;       ///< Pupil colour, native-endian RGB565
  uint16_t backColor;        ///< Back-of-eye colour, seen at extreme gaze
  uint16_t eyelidColor;      ///< Eyelid colour
  uint16_t irisColor;        ///< Iris colour used when no texture loads
  uint16_t scleraColor;      ///< Sclera colour used when no texture loads
  float pupilMin;            ///< Smallest pupil as a fraction of the iris
  float pupilMax;            ///< Largest pupil as a fraction of the iris
  bool tracking;             ///< Upper lid follows the iris
  float trackFactor;         ///< 1.0 minus squint; how far the lid rests down
  uint32_t gazeMax;          ///< Longest wait between major eye movements, us
  float irisSpin;            ///< Iris rotation in RPM, positive is clockwise
  float scleraSpin;          ///< Sclera rotation in RPM
  uint16_t irisStartAngle;   ///< Initial iris rotation, 0-1023 CCW
  uint16_t scleraStartAngle; ///< Initial sclera rotation, 0-1023 CCW
  uint16_t irisMirror;       ///< 0 or 1023; 1023 mirrors the iris texture
  uint16_t scleraMirror;     ///< 0 or 1023; 1023 mirrors the sclera
  bool eyelidMirror;         ///< Mirror the eyelid shape horizontally
  int fixate;                ///< Convergence toward the face, map pixels
  char irisFile[EYES_PATH_MAX];   ///< Iris texture path on the drive
  char scleraFile[EYES_PATH_MAX]; ///< Sclera texture path on the drive
  char upperFile[EYES_PATH_MAX];  ///< Upper eyelid bitmap path
  char lowerFile[EYES_PATH_MAX];  ///< Lower eyelid bitmap path
};

/**
 * @brief The few values that may differ between the two eyes.
 *
 * Everything else -- geometry, textures, eyelid shape -- is shared, because
 * there is only one set of polar maps and one copy of each texture in RAM. In
 * a .eye file these come from the "right" block (eye 0) and the "left" block
 * (eye 1), matching M4_Eyes, where eye 0 is the character's RIGHT eye and so
 * appears on the viewer's LEFT.
 */
struct EyesVariant {
  float irisSpin;            ///< Iris rotation in RPM for this eye
  float scleraSpin;          ///< Sclera rotation in RPM for this eye
  uint16_t irisStartAngle;   ///< Initial iris rotation, 0-1023 CCW
  uint16_t scleraStartAngle; ///< Initial sclera rotation, 0-1023 CCW
  uint16_t irisMirror;       ///< 0 or 1023; 1023 mirrors the iris
  uint16_t scleraMirror;     ///< 0 or 1023; 1023 mirrors the sclera
  bool eyelidMirror;         ///< Mirror the eyelid shape for this eye
};

/**
 * @brief Byte source for the BMP loaders.
 *
 * Abstracting this keeps the loaders testable on a host and independent of
 * whichever filesystem the board happens to use.
 */
class BmpReader {
public:
  virtual ~BmpReader() {}
  /**
   * @brief Move to an absolute byte offset.
   * @param pos Offset from the start of the file.
   * @return true if the seek succeeded.
   */
  virtual bool seek(uint32_t pos) = 0;
  /**
   * @brief Read bytes from the current position.
   * @param buf Destination buffer.
   * @param len Bytes requested.
   * @return Bytes actually read; 0 on failure.
   */
  virtual size_t read(void *buf, size_t len) = 0;
};

/**
 * @brief One or two animated eyes on a display.
 *
 * Single-instance by design: there is one flash chip, one set of polar maps
 * and, on RP2, one DVI output.
 */
class Adafruit_Monster_Eyes {
public:
  // -----------------------------------------------------------------------
  /** @name Construction
   *  Hand over the display your sketch already created. The eye count follows
   *  from how many panels you pass.
   */
  ///@{

  /**
   * @brief Drive one or two SPI TFT panels through Adafruit_GFX.
   *
   * Initialise the panels yourself before begin() -- Adafruit_GFX has no
   * common init call across drivers -- and pulse any shared reset line first
   * with resetPanels().
   *
   * @param panel0 First panel; becomes eye 0.
   * @param panel1 Second panel, or NULL for a single eye.
   */
  Adafruit_Monster_Eyes(Adafruit_SPITFT *panel0,
                        Adafruit_SPITFT *panel1 = NULL);

#if defined(ARDUINO_ARCH_RP2040)
  /**
   * @brief Drive a PicoDVI framebuffer. Do not call the object's begin().
   * @param dvi     Framebuffer object from the sketch.
   * @param numEyes Eyes to fit side by side in the one framebuffer.
   */
  Adafruit_Monster_Eyes(DVIGFX16 *dvi, uint8_t numEyes = 2);
#endif

#if defined(ARDUINO_ARCH_ESP32)
  /**
   * @brief Drive a parallel RGB666 panel. Do not call the object's begin().
   * @param gfx   Arduino_GFX display from the sketch.
   * @param scale Panel pixels per rendered pixel; 2 for a 480 panel, 3 for 720.
   */
  Adafruit_Monster_Eyes(Arduino_RGB_Display *gfx, uint8_t scale = 2);
#endif

  /**
   * @brief Drive any backend, including ones this library does not know about.
   * @param display Backend the sketch owns.
   */
  Adafruit_Monster_Eyes(Eyes_Display *display);

  ~Adafruit_Monster_Eyes();
  ///@}

  // -----------------------------------------------------------------------
  /** @name Startup */
  ///@{

  /**
   * @brief Bring everything up: display, storage, config, tables, textures.
   *
   * The order is load-bearing. Storage is mounted first, because bringing the
   * flash chip up disables XIP briefly and that cannot happen once a DVI
   * backend has core1 generating video. The display follows, since on a
   * framebuffer backend it is the largest allocation in the sketch; then the
   * polar maps, shrinking the eye and retrying if they will not fit; stripe
   * buffers are claimed before textures so the texture loader cannot starve
   * them; whatever heap is left becomes the texture budget.
   *
   * @return false on a fatal problem; see errorString(). The sketch decides
   *         what to do about it -- a library should not halt.
   */
  bool begin(void);

  /** @brief What went wrong. @return Message, or NULL if nothing did. */
  const char *errorString(void) const { return _error; }

  /**
   * @brief Print startup narration and frame profiling to a stream.
   *
   * Compile with -DMONSTER_EYES_LOG_LEVEL=0 to drop the strings entirely.
   *
   * @param stream Where to write, e.g. Serial.
   */
  void setVerbose(Stream &stream);

  /**
   * @brief Identify each panel with a solid fill at startup.
   * @param on true to run the test inside begin().
   */
  void setSelfTest(bool on) { _selfTest = on; }

  /**
   * @brief Account render versus transfer time, readable per frame.
   * @param on true to time transfers.
   */
  void setProfile(bool on);

  /**
   * @brief RP2 only: batch pixels into the SPI hardware and DMA them out.
   *
   * Only meaningful on the Adafruit_GFX TFT path, where it is worth a large
   * chunk of the frame rate, because the transfer then overlaps the next
   * column's render instead of following it. Needs the SCK pin so it can work
   * out which SPI block the panel is on; pass the same pin you gave
   * SPI.setSCK(). Ignored on other backends and other chips.
   *
   * Call before begin().
   *
   * @param sck0   SCK GPIO for panel 0.
   * @param sck1   SCK GPIO for panel 1, or -1 to reuse @p sck0.
   * @param useDma false to batch without DMA, which is slower but simpler to
   *               debug.
   */
  void setFastSPI(int sck0, int sck1 = -1, bool useDma = true);

  /**
   * @brief Pulse shared panel reset lines once, before any panel init.
   *
   * Construct panels with a reset pin of -1 and call this first, so no driver
   * pulses a shared line during its own init. With a shared line the second
   * panel's init would otherwise knock the first back to its power-on state.
   *
   * @param rst0 Reset GPIO, or -1 if tied to board reset.
   * @param rst1 Second reset GPIO if the panels differ, else -1.
   */
  static void resetPanels(int rst0, int rst1 = -1);
  ///@}

  // -----------------------------------------------------------------------
  /** @name Running */
  ///@{

  /** @brief Advance the animation and draw a frame. */
  void animate(void);

  /**
   * @brief Advance the animation without drawing.
   *
   * Split out so a sketch can interpose between the animator and the
   * renderer: call update(), adjust whatever you like, then draw().
   */
  void update(void);

  /** @brief Draw a frame from the current state. */
  void draw(void);

  /** @brief Frames drawn since the last frameRate() call. @return Rate in
   *  frames per second. */
  float frameRate(void);

  /** @brief Milliseconds of render time in the last frame. @return
   *  Milliseconds. */
  float renderMillis(void) const { return _lastRenderMs; }

  /** @brief Milliseconds of transfer time in the last frame; needs
   *  setProfile(). @return Milliseconds. */
  float transferMillis(void) const { return _lastTransferMs; }
  ///@}

  // -----------------------------------------------------------------------
  /** @name Live control
   *  Safe to call any time after begin(), once per frame or once ever. Each
   *  set*() takes ownership of that attribute until the matching release*().
   */
  ///@{

  /**
   * @brief Set pupil dilation, overriding the autonomous animator.
   * @param dilation 0.0 fully constricted to 1.0 fully dilated.
   */
  void setPupil(float dilation);

  /** @brief Hand the pupil back to the autonomous animator. */
  void releasePupil(void) { _pupilExternal = false; }

  /** @brief Current pupil dilation. @return 0.0 to 1.0. */
  float pupil(void) const;

  /**
   * @brief Point the gaze, overriding the autonomous animator.
   *
   * The eye still cannot look further than its geometry allows, so values are
   * scaled into the reachable disc rather than clipped to a square.
   *
   * @param x -1.0 hard left to 1.0 hard right.
   * @param y -1.0 down to 1.0 up.
   */
  void setGaze(float x, float y);

  /** @brief Hand the gaze back to the autonomous animator. */
  void releaseGaze(void) { _gazeExternal = false; }

  /** @brief Where the eye is looking. @return -1.0 to 1.0. */
  float gazeX(void) const;
  /** @brief Where the eye is looking. @return -1.0 to 1.0. */
  float gazeY(void) const;

  /** @brief Blink now, both eyes, at the usual speed. */
  void blink(void);

  /**
   * @brief Hold the lids at a given phase, overriding blinks entirely.
   * @param phase 0.0 fully open to 1.0 fully shut.
   */
  void setBlink(float phase);

  /** @brief Hand the lids back to the blink state machine. */
  void releaseBlink(void) { _blinkExternal = false; }

  /**
   * @brief Whether the eye blinks on its own.
   * @param on false to stop spontaneous blinking; blink() still works.
   */
  void setAutoBlink(bool on) { _autoBlink = on; }

  /**
   * @brief Whether the eye looks around on its own.
   * @param on false to leave the gaze wherever it was last put.
   */
  void setAutoGaze(bool on) { _autoGaze = on; }
  ///@}

  // -----------------------------------------------------------------------
  /** @name Appearance
   *  Colours take effect on the next frame. Geometry and texture paths are
   *  read by begin(), so set those first.
   */
  ///@{

  /**
   * @brief Rendered eye size. 0 fills whatever the display can give one eye.
   * @param px Width and height in pixels.
   */
  void setEyeSize(int px) { _settings.displaySize = px; }

  /**
   * @brief Eyeball radius. 0 derives it from the eye size.
   *
   * Leaving this, irisRadius and slitPupilRadius at their auto values is the
   * safest way to resize the eye: a mismatch between them is what lets the
   * iris wander out of frame.
   *
   * @param px Radius in screen pixels.
   */
  void setEyeRadius(int px) { _settings.eyeRadius = px; }

  /**
   * @brief Iris radius. 0 derives it from the eye size.
   * @param px Radius in screen pixels.
   */
  void setIrisRadius(int px) { _settings.irisRadius = px; }

  /**
   * @brief Slit pupil radius: goat, cat, dragon.
   * @param px Radius in screen pixels; 0 gives a round pupil, -1 derives it.
   */
  void setSlitPupilRadius(int px) { _settings.slitPupilRadius = px; }

  /**
   * @brief How much of the eyeball surface the polar maps cover.
   * @param c 0.05 to 1.0; raised automatically if the geometry needs it.
   */
  void setCoverage(float c) {
    _settings.coverage = _settings.coverageRequested = c;
  }

  /** @brief Iris colour when no texture is loaded. @param c RGB565. */
  void setIrisColor(uint16_t c);
  /** @brief Sclera colour when no texture is loaded. @param c RGB565. */
  void setScleraColor(uint16_t c);
  /** @brief Pupil colour. @param c RGB565. */
  void setPupilColor(uint16_t c) { _settings.pupilColor = c; }
  /** @brief Back-of-eye colour, seen at extreme gaze. @param c RGB565. */
  void setBackColor(uint16_t c) { _settings.backColor = c; }
  /** @brief Eyelid colour. @param c RGB565. */
  void setEyelidColor(uint16_t c) { _settings.eyelidColor = c; }

  /**
   * @brief Pupil size range as fractions of the iris.
   * @param minFrac Smallest pupil.
   * @param maxFrac Largest pupil.
   */
  void setPupilRange(float minFrac, float maxFrac);

  /**
   * @brief Whether the upper lid follows the iris.
   * @param on true to track.
   */
  void setTracking(bool on) { _settings.tracking = on; }

  /**
   * @brief How far the upper lid rests down over the eye.
   * @param s 0.0 wide open to 1.0 fully squinting.
   */
  void setSquint(float s) { _settings.trackFactor = 1.0f - s; }

  /**
   * @brief Iris rotation.
   * @param rpm Revolutions per minute; positive is clockwise.
   * @param eye Eye index, or -1 for both.
   */
  void setIrisSpin(float rpm, int eye = -1);

  /**
   * @brief Sclera rotation.
   * @param rpm Revolutions per minute; positive is clockwise.
   * @param eye Eye index, or -1 for both.
   */
  void setScleraSpin(float rpm, int eye = -1);

  /**
   * @brief Longest wait between major eye movements.
   * @param us Microseconds.
   */
  void setGazeMax(uint32_t us) { _settings.gazeMax = us; }

  /**
   * @brief Convergence of two eyes toward the face centre.
   * @param mapPixels Toe-in in polar-map pixels.
   */
  void setFixate(int mapPixels) { _settings.fixate = mapPixels; }

  /** @brief Iris texture path on the drive. @param path 24-bit BMP. */
  void setIrisTexture(const char *path);
  /** @brief Sclera texture path on the drive. @param path 24-bit BMP. */
  void setScleraTexture(const char *path);
  /** @brief Upper eyelid path on the drive. @param path 1-bit BMP. */
  void setUpperEyelid(const char *path);
  /** @brief Lower eyelid path on the drive. @param path 1-bit BMP. */
  void setLowerEyelid(const char *path);

  /**
   * @brief All the settings at once, for anything the setters do not cover.
   *
   * Read freely. Writing is only meaningful before begin() for geometry and
   * paths; colours can be written any time.
   *
   * @return Reference to the live settings.
   */
  EyesSettings &config(void) { return _settings; }

  /**
   * @brief Per-eye overrides.
   * @param eye Eye index.
   * @return Reference to that eye's variant.
   */
  EyesVariant &variant(uint8_t eye) {
    return _variant[eye < MONSTER_EYES_MAX_EYES ? eye : 0];
  }
  ///@}

  // -----------------------------------------------------------------------
  /** @name Storage */
  ///@{

  /**
   * @brief Where the JSON configuration lives on the drive.
   * @param path Defaults to "/config.eye".
   */
  void setConfigFile(const char *path) { _configFile = path; }

  /**
   * @brief Whether to mount the asset filesystem at all.
   * @param on false uses built-in defaults: a solid-colour eye, no eyelids.
   */
  void setStorageEnabled(bool on) { _storageEnabled = on; }

  /**
   * @brief Whether begin() offers the USB drive when the button is held.
   * @param on  false to skip the check, or to sequence it yourself.
   * @param pin Button GPIO; -1 uses BOOTSEL on RP2.
   */
  void setDriveModeEnabled(bool on, int pin = EYES_SAFE_MODE_PIN_DEFAULT) {
    _driveModeEnabled = on;
    _safeModePin = pin;
  }

  /**
   * @brief Which per-eye config block a single-eye build reads.
   * @param right true for the "right" block, false for "left".
   */
  void setSide(bool right) {
    _sideRight = right;
    _sideSet = true;
  }

  /** @brief Which block a single-eye build reads. @return true for right. */
  bool side(void) const { return _sideRight; }

  /**
   * @brief Is a button asking for USB drive mode?
   *
   * Call before anything else in setup() if you want to sequence drive mode
   * yourself; on RP2 it must happen before DVI claims core1 and the PIOs.
   *
   * @return true if drive mode is requested.
   */
  bool driveModeRequested(void);

  /**
   * @brief Export the flash over USB and never return.
   *
   * Reboots once host writes go quiet. The display is deliberately left off:
   * writing flash and driving a display cannot overlap.
   */
  void runDriveMode(void);

  /**
   * @brief Re-read the configuration file.
   * @param path Path, or NULL for the configured one.
   * @return false if the file is absent or unparseable; settings stay usable.
   */
  bool loadConfig(const char *path = NULL);
  ///@}

  // -----------------------------------------------------------------------
  /** @name State, for sync and other external drivers
   *  Raw internal units. Eyes_Sync uses these; most sketches want the live
   *  control API above instead.
   */
  ///@{

  /** @brief Gaze in polar-map pixels. @return X in map space. */
  float gazeMapX(void) const { return _frameEyeX; }
  /** @brief Gaze in polar-map pixels. @return Y in map space. */
  float gazeMapY(void) const { return _frameEyeY; }

  /**
   * @brief Set the gaze in polar-map pixels, taking ownership.
   * @param x X in map space.
   * @param y Y in map space.
   */
  void setGazeMap(float x, float y);

  /**
   * @brief Iris fraction: the renderer's own pupil unit.
   *
   * Note this is the INVERSE of dilation -- a larger iris means a smaller
   * pupil -- and it spans the configured pupilMin/pupilMax range rather than
   * 0 to 1. setPupil() does the conversion for you.
   *
   * @return Current iris fraction.
   */
  float irisFraction(void) const { return _irisValue; }

  /**
   * @brief Set the iris fraction directly, taking ownership of the pupil.
   * @param f Iris fraction.
   */
  void setIrisFraction(float f);

  /** @brief Blink phase of eye 0. @return 0.0 open to 1.0 shut. */
  float blinkPhase(void) const;

  /**
   * @brief Drive the blink phase directly, taking ownership of the lids.
   * @param f 0.0 open to 1.0 shut.
   */
  void setBlinkPhase(float f) { setBlink(f); }

  /**
   * @brief Offset applied to millis() for iris rotation.
   *
   * Rotation is derived from absolute time, so a board following another one
   * needs its clock aligned or the two irises sit at a fixed phase offset.
   *
   * @param offset Remote millis() minus ours.
   */
  void setTimeOffset(int32_t offset) { _clockOffset = offset; }

  /** @brief Current clock offset. @return Milliseconds. */
  int32_t timeOffset(void) const { return _clockOffset; }

  /** @brief Polar map radius in map pixels. @return Radius. */
  int mapRadius(void) const { return _mapRadius; }

  /** @brief Eyes being drawn. @return 1 or 2. */
  uint8_t eyeCount(void) const { return _numEyes; }

  /** @brief Rendered eye size. @return Pixels. */
  int eyeSize(void) const { return _size; }

  /** @brief The display backend, for anything backend-specific.
   *  @return Backend pointer. */
  Eyes_Display *display(void) const { return _display; }
  ///@}

private:
  /** @brief Per-eye animation state. */
  struct EyeState {
    float irisSpin;            ///< RPM * -1024; negative is clockwise
    float scleraSpin;          ///< RPM * -1024
    uint16_t irisStartAngle;   ///< 0-1023 CCW
    uint16_t scleraStartAngle; ///< 0-1023 CCW
    uint16_t irisAngle;        ///< Current rotation
    uint16_t scleraAngle;      ///< Current rotation
    uint8_t blinkState;        ///< NOBLINK, ENBLINK or DEBLINK
    uint32_t blinkDuration;    ///< Microseconds for this phase
    uint32_t blinkStartTime;   ///< When this phase began
    float blinkFactor;         ///< 0.0 open to 1.0 shut
    float eyeX;                ///< Gaze in map space, per eye to avoid tearing
    float eyeY;                ///< Gaze in map space
    float pupilFactor;         ///< Iris fraction for this frame
    float upperLidFactor;      ///< Smoothed upper lid position
    float lowerLidFactor;      ///< Smoothed lower lid position
  };

  void applyDefaults(void);
  void finalizeSettings(void);
  void seedVariants(void);
  void gazeRadiusInit(void);
  void updateGaze(uint32_t t);
  void updateIris(void);
  void updateBlinks(uint32_t t);
  void updateEye(uint8_t e, uint32_t t);
  void renderEye(uint8_t e) EYES_HOT;
  uint32_t eyeMillis(void) const;
  void fail(const char *why);

  // Tables (Adafruit_Monster_Eyes.cpp)
  bool tablesInit(void);
  void tablesFree(void);
  bool calcMap(void);
  bool calcDisplacement(void);
  float screen2map(int in) const;
  float map2screen(int in) const;

  // Assets (Eyes_Assets.cpp)
  bool storageBegin(void);
  void storageEnd(void);
  bool mediaLoad(int size, uint32_t texBudget);
  void applyConfigRoot(const void *variantPtr);
  void applyConfigVariant(const void *variantPtr, EyesVariant &v);
  /** @brief Convert a colour to the byte order the backend wants.
   *  @param v Native-endian RGB565. @return Output-order colour. */
  inline uint16_t out16(uint16_t v) const {
    return _swapBytes ? (uint16_t)__builtin_bswap16(v) : v;
  }

  Eyes_Display *_display; ///< Backend in use
  bool _ownsDisplay;      ///< true if we constructed it and must delete it
  bool _isTft;            ///< Backend is the Adafruit_GFX TFT path
  uint8_t _numEyes;       ///< Eyes being drawn
  bool _swapBytes;        ///< Backend wants big-endian pixels
  bool _begun;            ///< begin() has succeeded
  const char *_error;     ///< Last failure, or NULL

  EyesSettings _settings;                      ///< Live settings
  EyesVariant _variant[MONSTER_EYES_MAX_EYES]; ///< Per-eye overrides
  EyeState _eye[MONSTER_EYES_MAX_EYES];        ///< Per-eye animation state

  // Geometry
  int _size;         ///< Rendered eye size in pixels
  int _half;         ///< _size / 2
  float _gazeRadius; ///< Gaze travel radius in map pixels

  // Tables
  uint8_t *_displace;   ///< (size/2)^2 quadrant; 255 = outside eyeball
  uint8_t *_polarAngle; ///< mapRadius^2 quadrant of angles
  int8_t *_polarDist;   ///< mapRadius^2; >=0 sclera, <0 iris, -128 off
  int _mapRadius;       ///< Polar map radius in map pixels
  int _mapDiameter;     ///< Twice _mapRadius, for bounds checks

  // Media
  uint8_t *_lidBlock;          ///< One allocation holding all four lid tables
  uint8_t *_upperOpen;         ///< Upper lid per column, fully open
  uint8_t *_upperClosed;       ///< Upper lid per column, fully shut
  uint8_t *_lowerOpen;         ///< Lower lid per column, fully open
  uint8_t *_lowerClosed;       ///< Lower lid per column, fully shut
  const uint16_t *_irisData;   ///< Iris texture, or a 1x1 solid colour
  const uint16_t *_scleraData; ///< Sclera texture, or a 1x1 solid colour
  uint16_t _irisW;             ///< Iris texture width
  uint16_t _irisH;             ///< Iris texture height
  uint16_t _scleraW;           ///< Sclera texture width
  uint16_t _scleraH;           ///< Sclera texture height
  uint16_t _irisSolid;         ///< 1x1 fallback storage
  uint16_t _scleraSolid;       ///< 1x1 fallback storage
  bool _irisFromFile;          ///< A texture loaded, so colour setters cannot
  bool _scleraFromFile;        ///< A texture loaded, so colour setters cannot

  // Shared animation state
  bool _eyeInMotion;          ///< Mid-saccade
  float _eyeOldX;             ///< Saccade start, map space
  float _eyeOldY;             ///< Saccade start, map space
  float _eyeNewX;             ///< Saccade target, map space
  float _eyeNewY;             ///< Saccade target, map space
  uint32_t _eyeMoveStartTime; ///< When the current move began
  int32_t _eyeMoveDuration;   ///< How long it lasts
  uint32_t _lastSaccadeStop;  ///< When the last full saccade ended
  int32_t _saccadeInterval;   ///< Wait until the next full saccade
  uint32_t _timeOfLastBlink;  ///< When the last blink started
  uint32_t _timeToNextBlink;  ///< Wait until the next one
  float _frameEyeX;           ///< This frame's gaze, map space
  float _frameEyeY;           ///< This frame's gaze, map space

  // Autonomous iris scaling by fractal subdivision
  float _irisPrev[7];  ///< Previous value per subdivision level
  float _irisNext[7];  ///< Next value per subdivision level
  uint16_t _irisFrame; ///< Position in the subdivision cycle
  float _irisValue;    ///< Current iris fraction
  float _irisMin;      ///< Smallest iris fraction
  float _irisRange;    ///< Span of iris fractions

  // External control
  bool _gazeExternal;   ///< Something else owns the gaze
  bool _pupilExternal;  ///< Something else owns the pupil
  bool _blinkExternal;  ///< Something else owns the lids
  bool _autoBlink;      ///< Blink spontaneously
  bool _autoGaze;       ///< Look around spontaneously
  float _blinkForced;   ///< Phase to hold when _blinkExternal
  int32_t _clockOffset; ///< Added to millis() for rotation

  // Options
  const char *_configFile; ///< Path to the JSON configuration
  bool _storageEnabled;    ///< Mount the asset filesystem
  bool _driveModeEnabled;  ///< Offer the USB drive from begin()
  int _safeModePin;        ///< Button GPIO, or -1 for BOOTSEL
  bool _sideRight;         ///< Single-eye build reads the "right" block
  bool _sideSet;           ///< setSide() was called, so toe in a single eye
  uint32_t _frameMicros;   ///< micros() at the start of this frame
  bool _selfTest;          ///< Run the panel self-test in begin()
  bool _profile;           ///< Time transfers

  // Profiling
  uint32_t _frames;         ///< Frames since the last frameRate()
  uint32_t _lastRateReport; ///< micros() at the last frameRate()
  float _lastRenderMs;      ///< Render time, last frame
  float _lastTransferMs;    ///< Transfer time, last frame
};

#endif // _ADAFRUIT_MONSTER_EYES_H_