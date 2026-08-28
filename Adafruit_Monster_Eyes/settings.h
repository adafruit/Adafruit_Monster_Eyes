/**
 * @file settings.h
 * @brief User settings -- the only file most people need to edit.
 *
 * Everything here is wrapped in \#ifndef, which gives four layers, each
 * beating the one below:
 *   -# a -D flag on the compiler command line (what CI uses)
 *   -# whatever you uncomment in this file
 *   -# the board profile in @ref platform.h
 *   -# the generic defaults at the bottom of @ref platform.h
 *
 * Runtime appearance -- colours, iris size, textures, blink behaviour -- lives
 * in config.eye on the USB drive. The values here are the fallbacks used when
 * the drive has no config.
 */

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
//   EYE_PANEL_RGB666    parallel RGB666 round panel on an Adafruit Qualia S3

#define EYE_PANEL_AUTO 0    ///< Take the panel the board profile picks
#define EYE_PANEL_ST7789 1  ///< 240x240 or 240x320 square/rectangular TFT
#define EYE_PANEL_GC9A01A 2 ///< 240x240 round TFT
#define EYE_PANEL_ILI9341 3 ///< 240x320 TFT (Adafruit backend only)
#define EYE_PANEL_DVI 4     ///< HDMI/DVI output via PicoDVI (RP2 only)

#ifndef EYE_PANEL
/** Which panel is attached; see the EYE_PANEL_* values */
#define EYE_PANEL EYE_PANEL_AUTO
#endif

// One eye or two. 0 means "whatever suits this board" -- 1 where the adapter
// has a single chip select (EYESPI BFF) or the framebuffer leaves no room
// (DVI), 2 otherwise. A board profile cannot simply #define NUM_EYES, because
// this file is read first; eye.h resolves the 0 after platform.h has run.
#ifndef NUM_EYES
#define NUM_EYES 0 ///< Eyes to render; 1, 2, or 0 to follow the board profile
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
//  2b. QUALIA RGB666 PANEL (EYE_PANEL_RGB666 only)
// =========================================================================
//
// Only the round displays are supported
#define QUALIA_PANEL_21_480 0 ///< 2.1 inch 480x480 round
#define QUALIA_PANEL_28_480 1 ///< 2.8 inch 480x480 round
#define QUALIA_PANEL_40_720 2 ///< 4.0 inch 720x720 round

#ifndef QUALIA_PANEL
#define QUALIA_PANEL QUALIA_PANEL_21_480 ///< Which round display is attached
#endif

// The eye is rendered this many times smaller than the panel, then each pixel
// is replicated on the way out.
#ifndef EYE_SCALE
#if QUALIA_PANEL == QUALIA_PANEL_40_720
#define EYE_SCALE 3 ///< 720 panel from a 240 eye
#else
#define EYE_SCALE 2 ///< 480 panel from a 240 eye
#endif
#endif

#ifndef RGB_STRIPE_COLS
#define RGB_STRIPE_COLS 8 ///< Columns batched into one draw16bitRGBBitmap
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
#define DVI_RESOLUTION DVI_RES_320x240p60 ///< PicoDVI video mode
#endif

// =========================================================================
//  4. DEBUG MODE
// =========================================================================
// With EYE_DEBUG off, the sketch prints one line a second --
// the frame rate -- plus anything that actually went wrong. Startup,
// panel self-tests, memory reports and the render/transfer
// breakdown are all silent, and the boot delay is skipped.
#ifndef EYE_DEBUG
/** 1 enables startup logging, self-tests and frame profiling */
#define EYE_DEBUG 0
#endif

// The rest of the settings are advanced, likely won't need/want to be adjusted

// Panel size in pixels.
#ifndef TFT_W
#define TFT_W 240 ///< Panel width in pixels
#endif
#ifndef TFT_H
#define TFT_H 240 ///< Panel height in pixels
#endif

// SPI clock
#ifndef TFT_SPI_HZ
/** SPI clock for pixel data; panel init uses the driver default */
#define TFT_SPI_HZ 40000000
#endif

// Orientation
#ifndef TFT_ROTATION
#define TFT_ROTATION 0 ///< Adafruit_GFX rotation, 0-3 (Adafruit backend only)
#endif
#ifndef ESP_LCD_INVERT
/** Invert panel colours; most ST7789 and GC9A01A need this */
#define ESP_LCD_INVERT 1
#endif
#ifndef ESP_LCD_SWAP_XY
#define ESP_LCD_SWAP_XY 0 ///< Exchange rows and columns (esp_lcd backend)
#endif
#ifndef ESP_LCD_MIRROR_X
/** Mirror horizontally; GC9A01A stock orientation wants 1 */
#define ESP_LCD_MIRROR_X 0
#endif
#ifndef ESP_LCD_MIRROR_Y
#define ESP_LCD_MIRROR_Y 0 ///< Mirror vertically (esp_lcd backend)
#endif
#ifndef EYE_FORCE_ADAFRUIT_BACKEND
/** 1 uses Adafruit_GFX even where esp_lcd would serve */
#define EYE_FORCE_ADAFRUIT_BACKEND 0
#endif

// Delay before any hardware is touched. If the sketch faults later, the USB
// port still exists for this long after every reset, so the IDE can always
// reset the board for the next upload. Set to 0 once things are stable.
#ifndef STARTUP_GRACE_MS
#if EYE_DEBUG
/** Delay before touching hardware, so USB enumerates first */
#define STARTUP_GRACE_MS 3000
#else
/** Delay before touching hardware, so USB enumerates first */
#define STARTUP_GRACE_MS 0
#endif
#endif

// Bring-up tests. SELFTEST fills panel 0 red and panel 1 blue using the
// driver; PATHTEST repeats it through the actual render path. A panel dark in
// both means wiring; dark only in the second means the transfer path.
#ifndef DISPLAY_SELFTEST
/** Run the driver-level red/blue panel test at boot */
#define DISPLAY_SELFTEST EYE_DEBUG
#endif
#ifndef DISPLAY_PATHTEST
/** Repeat the panel test through the render path */
#define DISPLAY_PATHTEST EYE_DEBUG
#endif

// Report render vs transfer time once a second.
#ifndef PROFILE_FRAME
/** Report render versus transfer time once a second */
#define PROFILE_FRAME EYE_DEBUG
#endif

// Turn these off one at a time to bisect a startup hang.
#ifndef ENABLE_BOOTSEL_DRIVE
/** Allow BOOTSEL at reset to enter USB drive mode */
#define ENABLE_BOOTSEL_DRIVE 1
#endif
#ifndef ENABLE_STORAGE
/** Mount the asset filesystem; 0 uses built-in defaults */
#define ENABLE_STORAGE 1
#endif

// =========================================================================
//  4b. TWO-BOARD SYNC - PICODVI
// =========================================================================
//
// Run one eye per board and keep them in step over a STEMMA QT cable
//
// WIRING: one STEMMA QT cable between the two boards.
//
// SETUP: build both boards with NUM_EYES 1 and give them the same config and
// assets. The primary draws the right eye and the secondary the left, each
// reading its own block from config.eye -- see EYE_SYNC_SIDE_RIGHT below to
// swap them.
//
// WHAT IS SENT: gaze target, pupil dilation, blink phase, and the primary's
// clock so iris rotation stays in step. Everything else stays local.
//
// If packets stop arriving for EYE_SYNC_TIMEOUT_MS the secondary resumes
// animating on its own rather than freezing.
#define EYE_SYNC_OFF 0       ///< Standalone; no serial link
#define EYE_SYNC_PRIMARY 1   ///< Animates, and broadcasts state
#define EYE_SYNC_SECONDARY 2 ///< Renders the state it is sent

#ifndef EYE_SYNC
#define EYE_SYNC EYE_SYNC_OFF ///< Two-board sync role
#endif
// The STEMMA QT cable uses the default Wire pins:
// (Pico GPIO 4/5, Feather RP2040 DVI GPIO 2/3)
// The primary is the I2C controller and writes packets to the secondary's
// address.
//
#ifndef EYE_SYNC_I2C_ADDR
#define EYE_SYNC_I2C_ADDR 0x42 ///< Address the secondary answers to
#endif
#ifndef EYE_SYNC_I2C_WIRE
#define EYE_SYNC_I2C_WIRE Wire ///< Wire instance on the STEMMA QT port
#endif
#ifndef EYE_SYNC_I2C_HZ
#define EYE_SYNC_I2C_HZ 400000 ///< Bus speed; a packet is 13 bytes
#endif

#ifndef EYE_SYNC_SIDE_RIGHT
#if EYE_SYNC == EYE_SYNC_PRIMARY
#define EYE_SYNC_SIDE_RIGHT 1 ///< Primary draws the right eye
#else
#define EYE_SYNC_SIDE_RIGHT 0 ///< Secondary draws the left eye
#endif
#endif

#ifndef EYE_SYNC_TIMEOUT_MS
#define EYE_SYNC_TIMEOUT_MS 500 ///< Silence before the secondary free-runs
#endif

// =========================================================================
//  5. MEMORY
// =========================================================================

// Heap kept clear of textures, for stack and driver buffers.
#ifndef HEAP_RESERVE
/** Bytes kept clear of textures for stack and driver buffers */
#define HEAP_RESERVE 10000
#endif

// Smallest texture worth having. If the eye size requested does not leave
// this much over, setup() shrinks the eye rather than rendering it flat.
#ifndef MIN_TEXTURE_BUDGET
/** Below this the eye shrinks rather than render flat */
#define MIN_TEXTURE_BUDGET 10000
#endif

// Columns batched into one address window. Each window costs a fixed command
// sequence regardless of size, so batching is close to free frame rate. Must
// divide the eye size; the backend picks the largest divisor at or below this.
// Costs stripe * eyeSize * 4 bytes.
#ifndef TFT_STRIPE_COLS
/** Columns batched into one address window (Adafruit backend) */
#define TFT_STRIPE_COLS 16
#endif
#ifndef ESP_LCD_STRIPE_COLS
/** Columns batched into one draw_bitmap (esp_lcd backend) */
#define ESP_LCD_STRIPE_COLS 16
#endif

// RP2 only: batched SPI writes, and DMA so transfers overlap rendering.
#ifndef TFT_FAST_SPI
/** RP2 only: batch pixels straight into the SPI hardware */
#define TFT_FAST_SPI 1
#endif
#ifndef TFT_DMA
/** RP2 only: send columns by DMA so transfers overlap rendering */
#define TFT_DMA 1
#endif

// =========================================================================
//  6. ASSET LOCATION
// =========================================================================
// Pointers to config.eye on the CIRCUITPY drive
//

#ifndef CONFIG_FILENAME
/** Path to the JSON eye configuration on the drive */
#define CONFIG_FILENAME "/config.eye"
#endif
// Which per-eye block a single-eye build reads from a two-eye .eye file.
#ifndef EYE_SIDE
#if EYE_SYNC != EYE_SYNC_OFF
// Follows EYE_SYNC_SIDE_RIGHT, so a synced pair cannot end up with both
// boards reading the same block.
#define EYE_SIDE (EYE_SYNC_SIDE_RIGHT ? "right" : "left")
#else
#define EYE_SIDE "left" ///< Block a single-eye build reads from config.eye
#endif
#endif

#ifndef EYE_FIXATE
/** Convergence of two eyes toward the face centre, map pixels */
#define EYE_FIXATE 7
#endif
