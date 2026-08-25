/**
 * @file settings.h
 * @brief User settings -- the only file most people need to edit.
 *
 * Everything here is wrapped in \#ifndef, which gives three layers, each
 * beating the one below:
 *   -# a -D flag on the compiler command line (what CI uses)
 *   -# whatever you set in this file
 *   -# the board profile in @ref platform.h
 *
 * Runtime appearance -- colours, iris size, textures, blink behaviour -- lives
 * in config.eye on the USB drive. The values here are the fallbacks used when
 * the drive has no config.
 */
// =========================================================================
//  RP2 / ESP32 Eyes -- USER SETTINGS
// =========================================================================

#pragma once

// =========================================================================
//  1. PANEL
// =========================================================================
//
//   EYE_PANEL_AUTO      take the board profile's default
//   EYE_PANEL_ST7789    240x240 or 240x320 square/rectangular TFT
//   EYE_PANEL_GC9A01A   240x240 round TFT
//   EYE_PANEL_ILI9341   240x320 TFT (Adafruit backend only)
//   EYE_PANEL_DVI       HDMI/DVI output via PicoDVI (RP2 only)

#define EYE_PANEL_AUTO    0
#define EYE_PANEL_ST7789  1
#define EYE_PANEL_GC9A01A 2
#define EYE_PANEL_ILI9341 3
#define EYE_PANEL_DVI     4

#ifndef EYE_PANEL
#  define EYE_PANEL EYE_PANEL_AUTO
#endif

// One eye or two
#ifndef NUM_EYES
#  define NUM_EYES 2
#endif

// =========================================================================
//  2. WIRING -> TFT SPI
// =========================================================================
//
// Both panels share SCK, MOSI, DC and RST. Only chip select is per panel.
// Uncomment a line to override just that pin

#ifndef TFT_SCK
// #define TFT_SCK SCK
#endif
#ifndef TFT_MOSI
// #define TFT_MOSI MOSI
#endif
#ifndef TFT_DC
// #define TFT_DC 9
#endif
#ifndef TFT_RST
// #define TFT_RST 10 // -1 if tied to the board's reset line
#endif
#ifndef TFT_CS
// #define TFT_CS 11 // Panel 0
#endif
#ifndef TFT1_CS
// #define TFT1_CS 12 // Panel 1, only used when NUM_EYES is 2
#endif
#ifndef TFT_BACKLIGHT
// #define TFT_BACKLIGHT -1 // -1 if not switchable
#endif

// =========================================================================
//  3. DVI WIRING (EYE_PANEL_DVI only)
// =========================================================================
//
// The carrier board's pin map:
//   adafruit_feather_dvi_cfg  Feather RP2040 DVI
//   adafruit_dvibell_cfg      PiCowbell DVI
//   pico_sock_cfg             Pico DVI Sock
//   pimoroni_demo_hdmi_cfg    Pimoroni Pico DV

// #define DVI_PIN_CONFIG  adafruit_feather_dvi_cfg
#ifndef DVI_RESOLUTION
#  define DVI_RESOLUTION DVI_RES_320x240p60
#endif

// =========================================================================
//  4. DEBUG MODE
// =========================================================================
// With EYE_DEBUG off, the sketch prints one line a second --
// the frame rate -- plus anything that actually went wrong. Startup,
// panel self-tests, memory reports and the render/transfer
// breakdown are all silent, and the boot delay is skipped.
#ifndef EYE_DEBUG
#  define EYE_DEBUG 0
#endif

// The rest of the settings are advanced, likely won't need/want to be adjusted

// Panel size in pixels.
#ifndef TFT_W
#  define TFT_W 240
#endif
#ifndef TFT_H
#  define TFT_H 240
#endif

// SPI clock
#ifndef TFT_SPI_HZ
#  define TFT_SPI_HZ 40000000
#endif

// Orientation
#ifndef TFT_ROTATION
#  define TFT_ROTATION 0
#endif
#ifndef ESP_LCD_INVERT
#  define ESP_LCD_INVERT 1 // Most ST7789 and GC9A01A panels need this
#endif
#ifndef ESP_LCD_SWAP_XY
#  define ESP_LCD_SWAP_XY 0
#endif
#ifndef ESP_LCD_MIRROR_X
#  define ESP_LCD_MIRROR_X 0 // GC9A01A's stock orientation wants 1
#endif
#ifndef ESP_LCD_MIRROR_Y
#  define ESP_LCD_MIRROR_Y 0
#endif
#ifndef EYE_FORCE_ADAFRUIT_BACKEND
#  define EYE_FORCE_ADAFRUIT_BACKEND 0
#endif

// Delay before any hardware is touched. If the sketch faults later, the USB
// port still exists for this long after every reset, so the IDE can always
// reset the board for the next upload. Set to 0 once things are stable.
#ifndef STARTUP_GRACE_MS
#  if EYE_DEBUG
#    define STARTUP_GRACE_MS 3000
#  else
#    define STARTUP_GRACE_MS 0
#  endif
#endif

// Bring-up tests. SELFTEST fills panel 0 red and panel 1 blue using the
// driver; PATHTEST repeats it through the actual render path. A panel dark in
// both means wiring; dark only in the second means the transfer path.
#ifndef DISPLAY_SELFTEST
#  define DISPLAY_SELFTEST EYE_DEBUG
#endif
#ifndef DISPLAY_PATHTEST
#  define DISPLAY_PATHTEST EYE_DEBUG
#endif

// Report render vs transfer time once a second.
#ifndef PROFILE_FRAME
#  define PROFILE_FRAME EYE_DEBUG
#endif

// Turn these off one at a time to bisect a startup hang.
#ifndef ENABLE_BOOTSEL_DRIVE
#  define ENABLE_BOOTSEL_DRIVE 1
#endif
#ifndef ENABLE_STORAGE
#  define ENABLE_STORAGE 1
#endif

// =========================================================================
//  5. MEMORY
// =========================================================================

// Heap kept clear of textures, for stack and driver buffers.
#ifndef HEAP_RESERVE
#  define HEAP_RESERVE 10000
#endif

// Smallest texture worth having. If the eye size requested does not leave
// this much over, setup() shrinks the eye rather than rendering it flat.
#ifndef MIN_TEXTURE_BUDGET
#  define MIN_TEXTURE_BUDGET 10000
#endif

// Columns batched into one address window. Each window costs a fixed command
// sequence regardless of size, so batching is close to free frame rate. Must
// divide the eye size; the backend picks the largest divisor at or below this.
// Costs stripe * eyeSize * 4 bytes.
#ifndef TFT_STRIPE_COLS
#  define TFT_STRIPE_COLS 16
#endif
#ifndef ESP_LCD_STRIPE_COLS
#  define ESP_LCD_STRIPE_COLS 16
#endif

// RP2 only: batched SPI writes, and DMA so transfers overlap rendering.
#ifndef TFT_FAST_SPI
#  define TFT_FAST_SPI 1
#endif
#ifndef TFT_DMA
#  define TFT_DMA 1
#endif

// =========================================================================
//  6. FALLBACK EYE (used only when the drive has no config.eye)
// =========================================================================

#ifndef CONFIG_FILENAME
#  define CONFIG_FILENAME "/config.eye"
#endif
// Which per-eye block a single-eye build reads from a two-eye .eye file.
#ifndef EYE_SIDE
#  define EYE_SIDE "left"
#endif

// 0 means "fill the display". The three radii scale with it when left at
// their auto values, keeping the stock proportions at any size.
#ifndef DISPLAY_SIZE
#  define DISPLAY_SIZE 0
#endif
#ifndef EYE_RADIUS
#  define EYE_RADIUS 0 // 0 = displaySize/2 + 5
#endif
#ifndef IRIS_RADIUS
#  define IRIS_RADIUS 0 // 0 = 0.4583 * displaySize
#endif
#ifndef SLIT_PUPIL_RADIUS
#  define SLIT_PUPIL_RADIUS 0 // 0 = round pupil, -1 = auto slit
#endif
#ifndef COVERAGE
#  define COVERAGE 0.6f // Do not go far below 0.55
#endif

#ifndef PUPIL_COLOR
#  define PUPIL_COLOR 0x0000
#endif
#ifndef BACK_COLOR
#  define BACK_COLOR 0x5000
#endif
#ifndef EYELID_COLOR
#  define EYELID_COLOR 0x0000
#endif
#ifndef IRIS_COLOR
#  define IRIS_COLOR 0x001F
#endif
#ifndef SCLERA_COLOR
#  define SCLERA_COLOR 0xFFFF
#endif

#ifndef PUPIL_MIN
#  define PUPIL_MIN 0.05f
#endif
#ifndef PUPIL_MAX
#  define PUPIL_MAX 0.25f
#endif
// Eyelid tracking: the upper lid follows the iris, so the eye rests partly
// closed rather than staring. On by default, matching upstream M4_Eyes.
// TRACK_FACTOR is 1.0 - squint; config.eye sets "squint" instead.
#ifndef TRACKING
#  define TRACKING 1
#endif
#ifndef TRACK_FACTOR
#  define TRACK_FACTOR 0.5f
#endif
#ifndef GAZE_MAX
#  define GAZE_MAX 3000000
#endif
#ifndef IRIS_SPIN
#  define IRIS_SPIN -18.0f // RPM
#endif
#ifndef IRIS_START_ANGLE
#  define IRIS_START_ANGLE 512
#endif
#ifndef EYELID_MIRROR
#  define EYELID_MIRROR 1
#endif
// Two eyes toe in slightly, in polar-map pixels. Ignored for one eye.
#ifndef EYE_FIXATE
#  define EYE_FIXATE 7
#endif
