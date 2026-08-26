/**
 * @file platform.h
 * @brief Chip abstraction and per-board defaults.
 *
 * Everything in this project is plain C++ except a handful of calls no
 * Arduino core agrees on. They live here so porting to a new chip means
 * adding one block, not hunting through the render loop.
 *
 * The board profiles below supply defaults for anything @ref settings.h left
 * open -- which panel is likely attached, and which DVI carrier to assume.
 */

#pragma once
#include "settings.h"
#include <Arduino.h>

// Put a hot function in RAM instead of running it from flash. On RP2 the
// render loop otherwise fetches instructions through the XIP cache, which now
// competes with a DMA channel streaming pixels out of SRAM.
//
//   static void EYE_HOT_FN(renderEye)(int e) { ... }
//
#if defined(ARDUINO_ARCH_RP2040)
/**
 * @brief Place a hot function in RAM rather than flash.
 * @param name Function name to qualify.
 */
#define EYE_HOT_FN(name) __not_in_flash_func(name)
#else
// ESP32 has IRAM_ATTR, which would work here syntactically, but IRAM is
// scarce and renderEye is large -- enabling it can push a build over the
// IRAM limit. Left off; try `#define EYE_HOT_FN(name) IRAM_ATTR name` if
// profiling shows the render loop stalling on flash fetches.
/**
 * @brief No-op on chips where running from flash is not a bottleneck.
 * @param name Function name to qualify.
 */
#define EYE_HOT_FN(name) name
#endif

// =========================================================================
#if defined(ARDUINO_ARCH_RP2040) // Also defined for RP2350 by arduino-pico
// =========================================================================

#define PLATFORM_NAME "RP2" ///< Chip family name, for the startup banner

// -1 means "use the BOOTSEL button", which needs no extra hardware but halts
// XIP briefly to sample the QSPI CS pin. Set to a GPIO to use a real button.
#ifndef SAFE_MODE_PIN
/** GPIO whose press requests drive mode; -1 uses BOOTSEL */
#define SAFE_MODE_PIN -1
#endif

static inline uint32_t platformFreeHeap(void) { return rp2040.getFreeHeap(); }
static inline uint32_t platformLargestFreeBlock(void) {
  return rp2040.getFreeHeap();
}
// One flat SRAM, so nothing to steer.
static inline void *eyeMalloc(size_t n) { return malloc(n); }
static inline void platformReboot(void) { rp2040.reboot(); }
static inline uint32_t platformCpuHz(void) { return F_CPU; }

static inline bool platformSafeModeRequested(void) {
#if SAFE_MODE_PIN >= 0
  pinMode(SAFE_MODE_PIN, INPUT_PULLUP);
  delay(1);
  return digitalRead(SAFE_MODE_PIN) == LOW;
#else
  return BOOTSEL;
#endif
}

// =========================================================================
#elif defined(ARDUINO_ARCH_ESP32)
// =========================================================================

#define PLATFORM_NAME "ESP32" ///< Chip family name, for the startup banner
#include <esp_heap_caps.h>

// Most ESP32 boards wire the BOOT button to GPIO0.
#ifndef SAFE_MODE_PIN
/** GPIO whose press requests drive mode; -1 uses BOOTSEL */
#define SAFE_MODE_PIN 0
#endif

static inline uint32_t platformFreeHeap(void) { return ESP.getFreeHeap(); }

// PSRAM IS A TRAP FOR THIS WORKLOAD. With PSRAM configured, the default malloc
// sends large blocks to external RAM -- and the polar maps and iris texture are
// exactly that size. The render loop then does a random external read per
// pixel, which costs far more than the arithmetic around it.
//
// So eye data is allocated MALLOC_CAP_INTERNAL, and the texture budget is
// measured against internal RAM only. If internal RAM runs out the texture
// loader simply decimates further, which costs sharpness rather than speed.
static inline void *eyeMalloc(size_t n) {
  void *p = heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  return p ? p : malloc(n); // Fall back rather than fail outright
}

static inline uint32_t platformLargestFreeBlock(void) {
  return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL |
                                          MALLOC_CAP_8BIT);
}
static inline void platformReboot(void) { ESP.restart(); }
static inline uint32_t platformCpuHz(void) {
  return getCpuFrequencyMhz() * 1000000UL;
}

static inline bool platformSafeModeRequested(void) {
  pinMode(SAFE_MODE_PIN, INPUT_PULLUP);
  delay(1);
  return digitalRead(SAFE_MODE_PIN) == LOW;
}

// =========================================================================
#else
// =========================================================================
#error "Unsupported architecture -- add a block to platform.h"
#endif

// =========================================================================
//  BOARD PROFILES
// =========================================================================

// ---- Adafruit Feather RP2040 DVI ----------------------------------------
#if defined(ARDUINO_ADAFRUIT_FEATHER_RP2040_DVI)
/** Board profile that matched, for the startup banner */
#define EYE_BOARD_NAME "Feather RP2040 DVI"
#ifndef EYE_PANEL_DEFAULT
#define EYE_PANEL_DEFAULT EYE_PANEL_DVI ///< Panel this board most likely has
#endif
#ifndef DVI_PIN_CONFIG
/** PicoDVI carrier board pin map */
#define DVI_PIN_CONFIG adafruit_feather_dvi_cfg
#endif
#define EYE_BOARD_DEFAULT_EYES 1 ///< Eye count that suits this board

// ---- Adafruit Feather RP2040 --------------------------------------------
#elif defined(ARDUINO_ADAFRUIT_FEATHER_RP2040)
/** Board profile that matched, for the startup banner */
#define EYE_BOARD_NAME "Feather RP2040"
#ifndef EYE_PANEL_DEFAULT
#define EYE_PANEL_DEFAULT EYE_PANEL_ST7789 ///< Panel this board most likely has
#endif
#define EYE_BOARD_DEFAULT_EYES 2 ///< Eye count that suits this board

// ---- Adafruit Feather RP2350 ---------------------------------------------
#elif defined(ARDUINO_ADAFRUIT_FEATHER_RP2350)
/** Board profile that matched, for the startup banner */
#define EYE_BOARD_NAME "Feather RP2350"
#ifndef EYE_PANEL_DEFAULT
/** Panel this board most likely has */
#define EYE_PANEL_DEFAULT EYE_PANEL_GC9A01A
#endif
#define EYE_BOARD_DEFAULT_EYES 2 ///< Eye count that suits this board

// ---- Adafruit Metro RP2350 ----------------------------------------------
#elif defined(ARDUINO_ADAFRUIT_METRO_RP2350)
/** Board profile that matched, for the startup banner */
#define EYE_BOARD_NAME "Metro RP2350"
#ifndef EYE_PANEL_DEFAULT
/** Panel this board most likely has */
#define EYE_PANEL_DEFAULT EYE_PANEL_GC9A01A
#endif
#ifndef TFT1_CS
#define TFT1_CS 22 ///< Chip select for panel 1
#endif
#define EYE_BOARD_DEFAULT_EYES 2 ///< Eye count that suits this board

// ---- Adafruit Metro ESP32-S3 --------------------------------------------
#elif defined(ARDUINO_ADAFRUIT_METRO_ESP32S3)
/** Board profile that matched, for the startup banner */
#define EYE_BOARD_NAME "Metro ESP32-S3"
#ifndef EYE_PANEL_DEFAULT
#define EYE_PANEL_DEFAULT EYE_PANEL_ST7789 ///< Panel this board most likely has
#endif
#define EYE_BOARD_DEFAULT_EYES 2 ///< Eye count that suits this board

// ---- Adafruit QT Py RP2040 + EYESPI BFF ---------------------------------
#elif defined(ARDUINO_ADAFRUIT_QTPY_RP2040)
#define EYE_BOARD_NAME "QT Py RP2040 + EYESPI BFF" ///< Board profile name
#ifndef EYE_PANEL_DEFAULT
#define EYE_PANEL_DEFAULT EYE_PANEL_GC9A01A ///< Panel this board likely has
#endif
#define EYE_BOARD_DEFAULT_EYES 1 ///< One chip select on the BFF
#ifndef TFT_CS
#define TFT_CS PIN_SERIAL2_TX ///< GPIO 20, the pad marked TX
#endif
#ifndef TFT_DC
#define TFT_DC PIN_SERIAL2_RX ///< GPIO 5, the pad marked RX
#endif
#ifndef TFT_RST
#define TFT_RST -1 ///< Not wired on the BFF
#endif

// ---- Adafruit QT Py ESP32-S2 + EYESPI BFF -------------------------------
#elif defined(ARDUINO_ADAFRUIT_QTPY_ESP32S2)
#define EYE_BOARD_NAME "QT Py ESP32-S2 + EYESPI BFF" ///< Board profile name
#ifndef EYE_PANEL_DEFAULT
#define EYE_PANEL_DEFAULT EYE_PANEL_GC9A01A ///< Panel this board likely has
#endif
#define EYE_BOARD_DEFAULT_EYES 1 ///< One chip select on the BFF
#ifndef TFT_CS
#define TFT_CS TX ///< GPIO 5, the pad marked TX
#endif
#ifndef TFT_DC
#define TFT_DC RX ///< GPIO 16, the pad marked RX
#endif
#ifndef TFT_RST
#define TFT_RST -1 ///< Not wired on the BFF
#endif

// ---- Anything else ------------------------------------------------------
#else
/** Board profile that matched, for the startup banner */
#define EYE_BOARD_NAME "generic"
#ifndef EYE_PANEL_DEFAULT
#define EYE_PANEL_DEFAULT EYE_PANEL_ST7789 ///< Panel this board most likely has
#endif
#define EYE_BOARD_DEFAULT_EYES 2 ///< Eye count that suits this board
#endif

// Wiring shared by every board whose profile did not say otherwise. These
// work on the Feather and Metro form factors.
#ifndef TFT_SCK
#define TFT_SCK SCK ///< SPI clock pin
#endif
#ifndef TFT_MOSI
#define TFT_MOSI MOSI ///< SPI data-out pin
#endif
#ifndef TFT_DC
#define TFT_DC 9 ///< Data/command pin, shared by both panels
#endif
#ifndef TFT_RST
#define TFT_RST 10 ///< Panel reset pin, shared; -1 if tied to board reset
#endif
#ifndef TFT_CS
#define TFT_CS 11 ///< Chip select for panel 0
#endif
#ifndef TFT1_CS
#define TFT1_CS 12 ///< Chip select for panel 1
#endif
#ifndef TFT_BACKLIGHT
#define TFT_BACKLIGHT -1 ///< Backlight enable pin; -1 if not switchable
#endif

// DVI carrier fallback
// The carrier board's pin map:
//   adafruit_feather_dvi_cfg  Feather RP2040 DVI
//   adafruit_dvibell_cfg      PiCowbell DVI
//   pico_sock_cfg             Pico DVI Sock
//   pimoroni_demo_hdmi_cfg    Pimoroni Pico DV
#ifndef DVI_PIN_CONFIG
#define DVI_PIN_CONFIG pico_sock_cfg ///< PicoDVI carrier board pin map
#endif

// Both panels share everything but chip select
#ifndef TFT_SPI_PORT
#define TFT_SPI_PORT SPI ///< Arduino SPI object driving panel 0
#endif
#ifndef TFT1_SPI_PORT
#define TFT1_SPI_PORT TFT_SPI_PORT ///< Arduino SPI object driving panel 1
#endif
#ifndef TFT1_SCK
#define TFT1_SCK TFT_SCK ///< SPI clock pin for panel 1
#endif
#ifndef TFT1_MOSI
#define TFT1_MOSI TFT_MOSI ///< SPI data-out pin for panel 1
#endif
#ifndef TFT1_DC
#define TFT1_DC TFT_DC ///< Data/command pin for panel 1
#endif
#ifndef TFT1_RST
#define TFT1_RST TFT_RST ///< Reset pin for panel 1
#endif
