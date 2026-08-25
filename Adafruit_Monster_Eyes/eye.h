/**
 * @file eye.h
 * @brief Internal wiring: turns user settings into a concrete backend.
 *
 * You should not need to edit this file. User choices live in @ref settings.h
 * and per-board defaults in @ref platform.h; this header resolves those into
 * the macros and declarations the .cpp files compile against.
 *
 * It also declares the display backend interface that @ref display_tft.cpp,
 * @ref display_esp_lcd.cpp and @ref display_dvi.cpp each implement.
 */

#pragma once
#include "platform.h"
#include "settings.h"
#include <stddef.h>
#include <stdint.h>

// ===========================================================================
// BACKEND DERIVATION
// ===========================================================================
//
// One panel choice in settings.h becomes one backend here. The rule:
//
//   DVI                      -> PicoDVI      (RP2 only)
//   ST7789 / GC9A01A on ESP32 -> esp_lcd     (DMA, fastest)
//   anything else            -> Adafruit_GFX (portable)
//
// EYE_FORCE_ADAFRUIT_BACKEND overrides the middle rule, and ILI9341 falls
// through to Adafruit automatically because ESP-IDF has no core driver for it.

// Verbose output, compiled out entirely when EYE_DEBUG is 0. Genuine failures
// use Serial directly so they are reported either way.
#if EYE_DEBUG
/**
 * @brief Print a formatted diagnostic line; compiled out when EYE_DEBUG is 0.
 * @param ... printf-style format string and arguments.
 */
#define DBG(...) Serial.printf(__VA_ARGS__)
/**
 * @brief Print a diagnostic line; compiled out when EYE_DEBUG is 0.
 * @param s Text to print.
 */
#define DBGLN(s) Serial.println(s)
#else
/**
 * @brief No-op form of DBG(); EYE_DEBUG is 0.
 * @param ... Ignored.
 */
#define DBG(...)                                                               \
  do {                                                                         \
  } while (0)
/**
 * @brief No-op form of DBGLN(); EYE_DEBUG is 0.
 * @param s Ignored.
 */
#define DBGLN(s)                                                               \
  do {                                                                         \
  } while (0)
#endif

#define EYE_DISPLAY_DVI 0     ///< Backend: PicoDVI framebuffer
#define EYE_DISPLAY_TFT 1     ///< Backend: Adafruit_GFX SPI TFT
#define EYE_DISPLAY_ESP_LCD 2 ///< Backend: ESP-IDF esp_lcd with DMA

// Resolve the auto sentinels now that platform.h has supplied the board
// profile. NUM_EYES needs this indirection because a profile cannot simply
// #define it -- settings.h is read first, so its value would already be set.
#if NUM_EYES == 0
#undef NUM_EYES
#define NUM_EYES EYE_BOARD_DEFAULT_EYES ///< Eye count from the board profile
#endif

// Resolve EYE_PANEL_AUTO against the board profile.
#if EYE_PANEL == EYE_PANEL_AUTO
#undef EYE_PANEL
/** Which panel is attached; see the EYE_PANEL_* values */
#define EYE_PANEL EYE_PANEL_DEFAULT
#endif

#if EYE_PANEL == EYE_PANEL_DVI
#if !defined(ARDUINO_ARCH_RP2040)
#error "EYE_PANEL_DVI needs an RP2040 or RP2350 -- PicoDVI is RP2 only."
#endif
#define EYE_DISPLAY EYE_DISPLAY_DVI ///< Backend chosen from the panel and chip
#elif defined(ARDUINO_ARCH_ESP32) && !EYE_FORCE_ADAFRUIT_BACKEND &&            \
    ((EYE_PANEL == EYE_PANEL_ST7789) || (EYE_PANEL == EYE_PANEL_GC9A01A))
/** Backend chosen from the panel and chip */
#define EYE_DISPLAY EYE_DISPLAY_ESP_LCD
#else
#define EYE_DISPLAY EYE_DISPLAY_TFT ///< Backend chosen from the panel and chip
#endif

// Controller selection for whichever backend won.
#define TFT_DRIVER_ST7789 0  ///< Adafruit driver: ST7789
#define TFT_DRIVER_ILI9341 1 ///< Adafruit driver: ILI9341
#define TFT_DRIVER_GC9A01A 2 ///< Adafruit driver: GC9A01A

#if EYE_PANEL == EYE_PANEL_GC9A01A
/** Adafruit driver matching the selected panel */
#define TFT_DRIVER TFT_DRIVER_GC9A01A
#elif EYE_PANEL == EYE_PANEL_ILI9341
/** Adafruit driver matching the selected panel */
#define TFT_DRIVER TFT_DRIVER_ILI9341
#else
/** Adafruit driver matching the selected panel */
#define TFT_DRIVER TFT_DRIVER_ST7789
#endif

// ESP-IDF ships an ST7789 panel driver in core. GC9A01A is not in core, so the
// esp_lcd backend creates an ST7789 panel object -- the addressing commands are
// identical -- and sends the GC9A01A vendor init sequence itself.
#define ESP_LCD_DRV_ST7789 0 ///< esp_lcd panel: ST7789, shipped in ESP-IDF core
#define ESP_LCD_DRV_GC9A01A 1 ///< esp_lcd panel: GC9A01A, vendor init sent here

#if EYE_PANEL == EYE_PANEL_GC9A01A
/** esp_lcd panel matching the selected panel */
#define ESP_LCD_DRIVER ESP_LCD_DRV_GC9A01A
#else
/** esp_lcd panel matching the selected panel */
#define ESP_LCD_DRIVER ESP_LCD_DRV_ST7789
#endif

#ifndef ESP_LCD_HOST
#define ESP_LCD_HOST SPI2_HOST ///< SPI host the esp_lcd backend drives
#endif

// Framebuffer geometry, DVI only.
#define FB_WIDTH 320  ///< DVI framebuffer width in pixels
#define FB_HEIGHT 240 ///< DVI framebuffer height in pixels

// Is the RP2 batched-SPI burst actually in play?
#if (EYE_DISPLAY == EYE_DISPLAY_TFT) && TFT_FAST_SPI &&                        \
    defined(ARDUINO_ARCH_RP2040)
/** 1 when the RP2 batched-SPI burst is compiled in */
#define TFT_FAST_SPI_ACTIVE 1
#else
/** 1 when the RP2 batched-SPI burst is compiled in */
#define TFT_FAST_SPI_ACTIVE 0
#endif

// Byte order the render loop writes in. It costs nothing either way -- the
// swap happens once per texture at load and once per colour per frame -- so
// each backend asks for whatever its output path wants:
//
//   RP2 burst      native; 16-bit MSB-first frames emit the high byte first
//   Adafruit TFT   native; the driver swaps, and that branch measured faster
//   DVI            native; GFXcanvas16 is native uint16_t
//   esp_lcd        BIG-ENDIAN; the buffer goes to DMA verbatim
#if EYE_DISPLAY == EYE_DISPLAY_ESP_LCD
#define DISPLAY_BIG_ENDIAN 1 ///< 1 when the backend wants byte-swapped pixels
/**
 * @brief Convert a colour to the byte order this backend wants: byte-swapped
 * for the wire.
 * @param v Native-endian RGB565 colour.
 */
#define OUT16(v) __builtin_bswap16((uint16_t)(v))
#else
#define DISPLAY_BIG_ENDIAN 0 ///< 1 when the backend wants byte-swapped pixels
/**
 * @brief Convert a colour to the byte order this backend wants: unchanged.
 * @param v Native-endian RGB565 colour.
 */
#define OUT16(v) ((uint16_t)(v))
#endif

// ===========================================================================
// DISPLAY BACKEND
// ===========================================================================
//
// Contract, per frame:
//
//   displayFrameBegin();
//   for (e = 0; e < NUM_EYES; e++) {
//     displayEyeBegin(e);
//     for (x = 0; x < size; x++) {
//       uint16_t *p = displayColumn(e, x);
//       // write exactly `size` pixels, advancing p by displayColumnStride
//       displayColumnDone(e, x);
//     }
//     displayEyeEnd(e);
//   }
//   displayFrameEnd();
//
// The stride lets each backend choose its own memory layout and direction:
// the DVI backend hands back a pointer into the framebuffer with a negative
// row stride, while the TFT backend hands back a scratch buffer written in
// reverse so it can be shipped out top-to-bottom without a second pass.

/**
 * @brief Pulse every distinct reset pin once, before any panel is initialised.
 *
 * Panels are constructed without a reset pin so that no driver pulses the line
 * itself. With a SHARED reset, the second panel's init would otherwise knock
 * the first back to its power-on state -- the symptom being a panel that
 * flashes an image at boot and then stays dark while still receiving pixels.
 *
 * Duplicate pin numbers are pulsed only once.
 *
 * @param rstPins Reset GPIO per eye; entries below zero are skipped.
 * @param count   Number of entries in @p rstPins.
 */
void eyePanelReset(const int *rstPins, int count);

/**
 * @brief Bring up every display surface.
 * @return true on success; false leaves the sketch blinking an error.
 */
bool displayBegin(void);
/**
 * @brief Largest square one eye can occupy.
 *
 * With two eyes sharing a single framebuffer this is half the width, so a
 * config asking for more is clamped rather than overlapping its neighbour.
 *
 * @return Maximum eye width and height in pixels.
 */
int displayMaxEyeSize(void);
/**
 * @brief Fix the eye size and allocate per-column state.
 *
 * Call before textures claim the heap: on TFT this allocates the stripe
 * buffer, and if it were requested afterwards the texture loader could starve
 * it, leaving a running frame counter and a blank screen.
 *
 * @param size Eye width and height in pixels.
 */
void displaySetEyeSize(int size);
/**
 * @brief Fill every panel with a solid colour.
 * @param color Native-endian RGB565; the backend converts if it needs to.
 */
void displayClear(uint16_t color);

/** @brief Start a frame. */
void displayFrameBegin(void);

/**
 * @brief Start one eye; opens the bus transaction on SPI backends.
 * @param eye Eye index, 0 to NUM_EYES-1.
 */
void displayEyeBegin(int eye);

/**
 * @brief Where to write column @p x of eye @p eye.
 *
 * Write exactly the eye size in pixels, advancing by @ref
 * displayColumnStride after each one. The stride lets each backend choose its
 * own layout: the DVI backend returns a pointer into the framebuffer with a
 * negative row stride, while the SPI backends return the end of a stripe
 * buffer so the renderer fills it in the order the panel wants.
 *
 * @param eye Eye index, 0 to NUM_EYES-1.
 * @param x   Column index, 0 to the eye size minus one.
 * @return Buffer to write into, or NULL if no buffer could be allocated.
 */
uint16_t *displayColumn(int eye, int x);

/**
 * @brief Hand a finished column back; the backend may send it or batch it.
 * @param eye Eye index, 0 to NUM_EYES-1.
 * @param x   Column index, 0 to the eye size minus one.
 */
void displayColumnDone(int eye, int x);

/**
 * @brief Finish one eye, flushing anything still in flight.
 * @param eye Eye index, 0 to NUM_EYES-1.
 */
void displayEyeEnd(int eye);

/** @brief Finish the frame. */
void displayFrameEnd(void);
/**
 * @brief Fill panel 0 red and panel 1 blue using the driver's own fill.
 *
 * Answers three questions at once: is each panel responding, which physical
 * display is which index, and are the chip selects independent.
 */
void displaySelfTest(void);

/**
 * @brief Repeat the fill through the actual render path.
 *
 * A panel dark in both tests points at wiring; dark only in this one points at
 * the transfer path.
 *
 * @param size Eye size in pixels, as passed to displaySetEyeSize().
 */
void displayPathTest(int size);

/** @brief Pixels to advance between successive rows of a column. */
extern int displayColumnStride;

// Microseconds spent inside displayColumnDone() since last cleared, i.e. time
// pushing pixels at the panel. Zero on DVI, where a column is already in the
// framebuffer and there is nothing to push.
extern volatile uint32_t displayBusyMicros; ///< Microseconds spent pushing

// ===========================================================================
// SETTINGS
// ===========================================================================

#define EYE_PATH_MAX 64 ///< Longest asset path accepted from a config file

/** @brief Everything a config.eye file can change about the eye. */
struct EyeSettings {
  int displaySize;             ///< Eye width and height in pixels
  int eyeRadius;               ///< Eyeball radius in screen pixels
  int irisRadius;              ///< Iris radius in screen pixels
  int slitPupilRadius;         ///< Slit pupil radius; 0 gives a round pupil
  float coverage;              ///< Effective, possibly raised by finalize()
  float coverageRequested;     ///< What the config actually asked for
  uint16_t pupilColor;         ///< Pupil colour, native-endian RGB565
  uint16_t backColor;          ///< Back-of-eye colour, seen at extreme gaze
  uint16_t eyelidColor;        ///< Eyelid colour
  uint16_t irisColor;          ///< Iris colour used when no texture loads
  uint16_t scleraColor;        ///< Sclera colour used when no texture loads
  float pupilMin;              ///< Smallest pupil as a fraction of the iris
  float pupilMax;              ///< Largest pupil as a fraction of the iris
  bool tracking;               ///< Upper lid follows the iris
  float trackFactor;           ///< 1.0 minus squint; how far the lid rests down
  uint32_t gazeMax;            ///< Longest wait between major eye movements, us
  float irisSpin;              ///< Iris rotation in RPM, positive is clockwise
  float scleraSpin;            ///< Sclera rotation in RPM
  uint16_t irisStartAngle;     ///< Initial iris rotation, 0-1023 CCW
  uint16_t scleraStartAngle;   ///< Initial sclera rotation, 0-1023 CCW
  uint16_t irisMirror;         ///< 0 or 1023; 1023 mirrors the iris texture
  uint16_t scleraMirror;       ///< 0 or 1023; 1023 mirrors the sclera texture
  bool eyelidMirror;           ///< Mirror the eyelid shape horizontally
  char irisFile[EYE_PATH_MAX]; ///< Iris texture path on the drive
  char scleraFile[EYE_PATH_MAX]; ///< Sclera texture path on the drive
  char upperFile[EYE_PATH_MAX];  ///< Upper eyelid bitmap path
  char lowerFile[EYE_PATH_MAX];  ///< Lower eyelid bitmap path
};

extern EyeSettings settings; ///< Live settings, shared by every eye

// The handful of values that may legitimately differ between two eyes.
// Everything else -- geometry, textures, eyelid shape -- is shared, because
// there is only one set of polar maps and one copy of each texture in RAM.
// In a .eye file these come from the "right" block (eye 0) and the "left"
// block (eye 1), matching the original M4_Eyes naming, where eye 0 is the
// character's RIGHT eye and therefore appears on the viewer's LEFT.
/** @brief The few values that may differ between the two eyes. */
struct EyeVariant {
  float irisSpin;            ///< Iris rotation in RPM for this eye
  float scleraSpin;          ///< Sclera rotation in RPM for this eye
  uint16_t irisStartAngle;   ///< Initial iris rotation, 0-1023 CCW
  uint16_t scleraStartAngle; ///< Initial sclera rotation, 0-1023 CCW
  uint16_t irisMirror;       ///< 0 or 1023; 1023 mirrors the iris
  uint16_t scleraMirror;     ///< 0 or 1023; 1023 mirrors the sclera
  bool eyelidMirror;         ///< Mirror the eyelid shape for this eye
};

extern EyeVariant eyeVariant[NUM_EYES]; ///< Per-eye overrides

/** @brief Populate @ref settings from the compile-time defaults. */
void eyeSettingsDefaults(void);
/**
 * @brief Overlay values from a JSON config file onto @ref settings.
 *
 * Missing keys keep their current value, so this is safe to call on top of the
 * defaults. A single-eye build also applies the @ref EYE_SIDE block, so
 * two-eye .eye files still do something sensible.
 *
 * @param filename Path to the JSON configuration on the drive.
 * @return false if the file is absent or unparseable; settings stay usable.
 */
bool eyeSettingsLoad(const char *filename);
/**
 * @brief Clamp settings, resolve auto values and guarantee a usable gaze.
 *
 * Resolves the 0/-1 sentinels for eye, iris and slit-pupil radius, and raises
 * @c coverage if the geometry would otherwise leave the eye no room to look
 * around.
 */
void eyeSettingsFinalize(void);

// ===========================================================================
// STORAGE
// ===========================================================================

/**
 * @brief Mount the FAT volume holding config.eye and the bitmaps.
 * @return true if the volume mounted; false leaves built-in defaults in use.
 */
bool eyeStorageBegin(void);
/** @brief Stop reading the filesystem so flash stays quiet while rendering. */
void eyeStorageEnd(void);
/**
 * @brief Has the user asked for the USB drive instead of the eye?
 * @return true if the safe-mode button or BOOTSEL is held.
 */
bool eyeStorageDriveModeRequested(void);
/**
 * @brief Export flash over USB and never return.
 *
 * DVI and the panels are not started in this mode -- see the comment in
 * eye_support.cpp for why writing flash and driving a display cannot overlap.
 * Reboots once host writes go quiet.
 */
void eyeStorageRunDriveMode(void);

// ===========================================================================
// TABLES
// ===========================================================================

extern uint8_t *displace;   ///< (size/2)^2 quadrant; 255 = outside eyeball
extern uint8_t *polarAngle; ///< mapRadius^2 quadrant of angles
extern int8_t *polarDist;   ///< mapRadius^2; >=0 sclera, <0 iris, -128 off
extern int mapRadius;       ///< Polar map radius in map pixels
extern int mapDiameter;     ///< Twice mapRadius, for bounds checks

/**
 * @brief Build the polar and displacement maps from the current settings.
 * @return false if allocation failed; the caller may shrink the eye and retry.
 */
bool eyeTablesInit(void);
/** @brief Release the polar and displacement maps. */
void eyeTablesFree(void);
/**
 * @brief Convert a length in screen pixels to polar-map pixels.
 * @param in Length in screen pixels.
 * @return The equivalent length in polar-map pixels.
 */
float screen2map(int in);
/**
 * @brief Inverse of screen2map().
 * @param in Length in polar-map pixels.
 * @return The equivalent length in screen pixels.
 */
float map2screen(int in);

// ===========================================================================
// MEDIA
// ===========================================================================

extern uint8_t *upperOpen;       ///< Upper lid position per column, fully open
extern uint8_t *upperClosed;     ///< Upper lid position per column, fully shut
extern uint8_t *lowerOpen;       ///< Lower lid position per column, fully open
extern uint8_t *lowerClosed;     ///< Lower lid position per column, fully shut
extern const uint16_t *irisData; ///< Iris texture, or a 1x1 solid colour
extern const uint16_t *scleraData; ///< Sclera texture, or a 1x1 solid colour

/** @brief Iris texture width in pixels. @return Width, at least 1. */
uint16_t irisWidth(void);
/** @brief Iris texture height in pixels. @return Height, at least 1. */
uint16_t irisHeight(void);
/** @brief Sclera texture width in pixels. @return Width, at least 1. */
uint16_t scleraWidth(void);
/** @brief Sclera texture height in pixels. @return Height, at least 1. */
uint16_t scleraHeight(void);

/**
 * @brief Load eyelid tables and textures for an eye of @p size pixels.
 *
 * Every asset is optional. A missing texture becomes a 1x1 solid colour, which
 * the renderer samples correctly and which still yields a properly sized,
 * dilating pupil; a missing eyelid leaves the sweep tables at their init
 * values, which reads as no eyelid.
 *
 * @param size       Eye size in pixels.
 * @param texBudget  Bytes textures may consume in total. Oversized images are
 *                   decimated to fit rather than rejected.
 * @return false only if the eyelid tables could not be allocated.
 */
bool eyeMediaLoad(int size, uint32_t texBudget);

// ===========================================================================
// BMP
// ===========================================================================

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
 * @brief Load a 1-bit eyelid bitmap into a pair of sweep tables.
 *
 * For each column the topmost and bottommost lit pixel become the fully open
 * and fully shut lid positions, flipped into the renderer's frame where +Y is
 * up. The image is scaled to @p size, so any source dimensions work.
 *
 * @param r           Source of BMP bytes.
 * @param openTable   Receives the fully-open position per column.
 * @param closedTable Receives the fully-shut position per column.
 * @param size        Eye size in pixels; both tables hold this many entries.
 * @param isUpper     true for the upper lid, false for the lower.
 * @return false if the image is not an uncompressed 1-bit BMP.
 */
bool bmpLoadEyelid(BmpReader &r, uint8_t *openTable, uint8_t *closedTable,
                   int size, bool isUpper);
/**
 * @brief Load a 24-bit BMP as an RGB565 texture, decimating it to fit.
 *
 * Oversized images lose resolution rather than being rejected, so any source
 * works on any board.
 *
 * @param r        Source of BMP bytes.
 * @param data     Receives a malloc'd buffer the caller owns.
 * @param width    Receives the resulting width.
 * @param height   Receives the resulting height.
 * @param maxBytes Largest buffer the caller can afford.
 * @return false if the image is not an uncompressed 24-bit BMP, or if even
 *         the smallest decimation exceeds @p maxBytes.
 */
bool bmpLoadTexture(BmpReader &r, uint16_t **data, uint16_t *width,
                    uint16_t *height, uint32_t maxBytes);
