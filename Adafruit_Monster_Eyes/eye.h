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
#include <stdint.h>
#include <stddef.h>
#include "settings.h"
#include "platform.h"

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
#  define DBG(...) Serial.printf(__VA_ARGS__)
#  define DBGLN(s) Serial.println(s)
#else
#  define DBG(...) \
    do {           \
    } while (0)
#  define DBGLN(s) \
    do {           \
    } while (0)
#endif

#define EYE_DISPLAY_DVI     0
#define EYE_DISPLAY_TFT     1
#define EYE_DISPLAY_ESP_LCD 2

// Resolve EYE_PANEL_AUTO against the board profile.
#if EYE_PANEL == EYE_PANEL_AUTO
#  undef EYE_PANEL
#  define EYE_PANEL EYE_PANEL_DEFAULT
#endif

#if EYE_PANEL == EYE_PANEL_DVI
#  if !defined(ARDUINO_ARCH_RP2040)
#    error "EYE_PANEL_DVI needs an RP2040 or RP2350 -- PicoDVI is RP2 only."
#  endif
#  define EYE_DISPLAY EYE_DISPLAY_DVI
#elif defined(ARDUINO_ARCH_ESP32) && !EYE_FORCE_ADAFRUIT_BACKEND && \
    ((EYE_PANEL == EYE_PANEL_ST7789) || (EYE_PANEL == EYE_PANEL_GC9A01A))
#  define EYE_DISPLAY EYE_DISPLAY_ESP_LCD
#else
#  define EYE_DISPLAY EYE_DISPLAY_TFT
#endif

// Controller selection for whichever backend won.
#define TFT_DRIVER_ST7789  0
#define TFT_DRIVER_ILI9341 1
#define TFT_DRIVER_GC9A01A 2

#if EYE_PANEL == EYE_PANEL_GC9A01A
#  define TFT_DRIVER TFT_DRIVER_GC9A01A
#elif EYE_PANEL == EYE_PANEL_ILI9341
#  define TFT_DRIVER TFT_DRIVER_ILI9341
#else
#  define TFT_DRIVER TFT_DRIVER_ST7789
#endif

// ESP-IDF ships an ST7789 panel driver in core. GC9A01A is not in core, so the
// esp_lcd backend creates an ST7789 panel object -- the addressing commands are
// identical -- and sends the GC9A01A vendor init sequence itself.
#define ESP_LCD_DRV_ST7789  0
#define ESP_LCD_DRV_GC9A01A 1

#if EYE_PANEL == EYE_PANEL_GC9A01A
#  define ESP_LCD_DRIVER ESP_LCD_DRV_GC9A01A
#else
#  define ESP_LCD_DRIVER ESP_LCD_DRV_ST7789
#endif

#ifndef ESP_LCD_HOST
#  define ESP_LCD_HOST SPI2_HOST
#endif

// Framebuffer geometry, DVI only.
#define FB_WIDTH  320
#define FB_HEIGHT 240

// Is the RP2 batched-SPI burst actually in play?
#if (EYE_DISPLAY == EYE_DISPLAY_TFT) && TFT_FAST_SPI && \
    defined(ARDUINO_ARCH_RP2040)
#  define TFT_FAST_SPI_ACTIVE 1
#else
#  define TFT_FAST_SPI_ACTIVE 0
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
#  define DISPLAY_BIG_ENDIAN 1
#  define OUT16(v)           __builtin_bswap16((uint16_t)(v))
#else
#  define DISPLAY_BIG_ENDIAN 0
#  define OUT16(v)           ((uint16_t)(v))
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

/** @brief Start one eye; opens the bus transaction on SPI backends. */
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
 * @return Buffer to write into, or NULL if no buffer could be allocated.
 */
uint16_t *displayColumn(int eye, int x);

/** @brief Hand column @p x back; the backend may send it now or batch it. */
void displayColumnDone(int eye, int x);

/** @brief Finish one eye, flushing anything still in flight. */
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
extern volatile uint32_t displayBusyMicros;

// ===========================================================================
// SETTINGS
// ===========================================================================

#define EYE_PATH_MAX 64

struct EyeSettings {
  int displaySize, eyeRadius, irisRadius, slitPupilRadius;
  float coverage;          // Effective, possibly raised by finalize()
  float coverageRequested; // What the config actually asked for
  uint16_t pupilColor, backColor, eyelidColor, irisColor, scleraColor;
  float pupilMin, pupilMax;
  bool tracking;
  float trackFactor;
  uint32_t gazeMax;
  float irisSpin, scleraSpin; // RPM, positive = clockwise to viewer
  uint16_t irisStartAngle, scleraStartAngle;
  uint16_t irisMirror, scleraMirror; // 0 or 1023
  bool eyelidMirror;
  char irisFile[EYE_PATH_MAX], scleraFile[EYE_PATH_MAX];
  char upperFile[EYE_PATH_MAX], lowerFile[EYE_PATH_MAX];
};

extern EyeSettings settings;

// The handful of values that may legitimately differ between two eyes.
// Everything else -- geometry, textures, eyelid shape -- is shared, because
// there is only one set of polar maps and one copy of each texture in RAM.
// In a .eye file these come from the "right" block (eye 0) and the "left"
// block (eye 1), matching the original M4_Eyes naming, where eye 0 is the
// character's RIGHT eye and therefore appears on the viewer's LEFT.
struct EyeVariant {
  float irisSpin, scleraSpin;
  uint16_t irisStartAngle, scleraStartAngle;
  uint16_t irisMirror, scleraMirror;
  bool eyelidMirror;
};

extern EyeVariant eyeVariant[NUM_EYES];

/** @brief Populate @ref settings from the compile-time defaults. */
void eyeSettingsDefaults(void);
/**
 * @brief Overlay values from a JSON config file onto @ref settings.
 *
 * Missing keys keep their current value, so this is safe to call on top of the
 * defaults. A single-eye build also applies the @ref EYE_SIDE block, so
 * two-eye .eye files still do something sensible.
 *
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

/** @brief Mount the FAT volume holding config.eye and the BMPs. */
bool eyeStorageBegin(void);
/** @brief Stop reading the filesystem so flash stays quiet while rendering. */
void eyeStorageEnd(void);
/** @brief True if the user asked for the USB drive instead of the eye. */
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

extern uint8_t *displace;   // (size/2)^2, 255 = outside eyeball
extern uint8_t *polarAngle; // mapRadius^2
extern int8_t *polarDist;   // mapRadius^2, >=0 sclera, <0 iris, -128 = off
extern int mapRadius, mapDiameter;

/**
 * @brief Build the polar and displacement maps from the current settings.
 * @return false if allocation failed; the caller may shrink the eye and retry.
 */
bool eyeTablesInit(void);
/** @brief Release the polar and displacement maps. */
void eyeTablesFree(void);
/** @brief Convert a length in screen pixels to polar-map pixels. */
float screen2map(int in);
/** @brief Inverse of screen2map(). */
float map2screen(int in);

// ===========================================================================
// MEDIA
// ===========================================================================

extern uint8_t *upperOpen, *upperClosed, *lowerOpen, *lowerClosed;
extern const uint16_t *irisData, *scleraData;

uint16_t irisWidth(void), irisHeight(void);
uint16_t scleraWidth(void), scleraHeight(void);

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

class BmpReader {
public:
  virtual ~BmpReader() {}
  virtual bool seek(uint32_t pos) = 0;
  virtual size_t read(void *buf, size_t len) = 0;
};

bool bmpLoadEyelid(BmpReader &r, uint8_t *openTable, uint8_t *closedTable,
                   int size, bool isUpper);
bool bmpLoadTexture(BmpReader &r, uint16_t **data, uint16_t *width,
                    uint16_t *height, uint32_t maxBytes);
