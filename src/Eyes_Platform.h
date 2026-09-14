/**
 * @file Eyes_Platform.h
 * @brief Chip abstraction and library logging. Internal to the library.
 *
 * Everything in this library is plain C++ except a handful of calls no Arduino
 * core agrees on. They live here so porting to a new chip means adding one
 * block, not hunting through the render loop.
 *
 * The board profiles that used to live alongside these (which panel a board
 * probably has, which DVI carrier to assume) are gone: the sketch now
 * constructs its own display object, so those guesses have no job to do.
 */

#ifndef _EYES_PLATFORM_H_
#define _EYES_PLATFORM_H_

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

/** Largest number of eyes one instance can drive. */
#ifndef MONSTER_EYES_MAX_EYES
#define MONSTER_EYES_MAX_EYES 2
#endif

/** Bytes kept clear of textures, for stack and driver buffers. */
#ifndef MONSTER_EYES_HEAP_RESERVE
#define MONSTER_EYES_HEAP_RESERVE 10000
#endif

// Smallest texture worth having. If the requested eye size does not leave this
// much over, begin() shrinks the eye rather than rendering it flat.
/** Below this the eye shrinks rather than render a flat texture. */
#ifndef MONSTER_EYES_MIN_TEXTURE_BUDGET
#define MONSTER_EYES_MIN_TEXTURE_BUDGET 10000
#endif

// ---------------------------------------------------------------------------
//  LOGGING
// ---------------------------------------------------------------------------
//
// DBG() used to be gated by a compile-time EYE_DEBUG in the sketch. A library
// .cpp cannot see a sketch's #defines, so verbosity is now a runtime choice:
// Adafruit_Monster_Eyes::setVerbose() points this at a Stream.
//
// MONSTER_EYES_LOG_LEVEL still gates whether the format strings are compiled
// in at all, for anyone who needs the flash back:
//   0  silent
//   1  failures only
//   2  full startup narration and frame profiling (default)
// Override with a -D build flag.

/** Stream diagnostics are written to, or NULL for silence. */
extern Stream *eyesLogStream;

#ifndef MONSTER_EYES_LOG_LEVEL
#define MONSTER_EYES_LOG_LEVEL 2 ///< 0 silent, 1 failures, 2 verbose
#endif

#if MONSTER_EYES_LOG_LEVEL >= 1
/**
 * @brief Report something that actually went wrong.
 * @param ... printf-style format string and arguments.
 */
#define EYES_ERR(...)                                                          \
  do {                                                                         \
    if (eyesLogStream)                                                         \
      eyesLogStream->printf(__VA_ARGS__);                                      \
  } while (0)
#else
/** @brief No-op; MONSTER_EYES_LOG_LEVEL is 0. */
#define EYES_ERR(...)                                                          \
  do {                                                                         \
  } while (0)
#endif

#if MONSTER_EYES_LOG_LEVEL >= 2
/**
 * @brief Print a formatted diagnostic line when a log stream is set.
 * @param ... printf-style format string and arguments.
 */
#define EYES_DBG(...)                                                          \
  do {                                                                         \
    if (eyesLogStream)                                                         \
      eyesLogStream->printf(__VA_ARGS__);                                      \
  } while (0)
#else
/** @brief No-op form of EYES_DBG(). */
#define EYES_DBG(...)                                                          \
  do {                                                                         \
  } while (0)
#endif

// ---------------------------------------------------------------------------
//  HOT FUNCTIONS
// ---------------------------------------------------------------------------
//
// Put the render loop in RAM instead of running it from flash. On RP2 it
// otherwise fetches instructions through the XIP cache, which competes with a
// DMA channel streaming pixels out of SRAM.
//
// Spelled as an explicit section rather than the core's __not_in_flash_func():
// that macro stringizes the function name into the section name, and a C++
// member name would put "::" in there.
#if defined(ARDUINO_ARCH_RP2040)
/** Place a function in RAM rather than flash (RP2 only). */
#define EYES_HOT __attribute__((section(".time_critical.monster_eyes")))
#else
// ESP32 has IRAM_ATTR, which would work here syntactically, but IRAM is scarce
// and the renderer is large -- enabling it can push a build over the IRAM
// limit. Left off; try IRAM_ATTR if profiling shows flash-fetch stalls.
/** No-op on chips where running from flash is not a bottleneck. */
#define EYES_HOT
#endif

// ===========================================================================
#if defined(ARDUINO_ARCH_RP2040) // Also defined for RP2350 by arduino-pico
// ===========================================================================

#define EYES_PLATFORM_NAME "RP2" ///< Chip family, for the startup banner

/** @brief Free heap in bytes. @return Bytes free. */
static inline uint32_t eyesFreeHeap(void) { return rp2040.getFreeHeap(); }

/** @brief Largest single allocation available. @return Bytes. */
static inline uint32_t eyesLargestFreeBlock(void) {
  return rp2040.getFreeHeap();
}

/**
 * @brief Allocate memory the render loop will touch per pixel.
 *
 * One flat SRAM here, so nothing to steer.
 *
 * @param n Bytes to allocate.
 * @return Pointer, or NULL.
 */
static inline void *eyesMalloc(size_t n) { return malloc(n); }

/** @brief Restart the board. */
static inline void eyesReboot(void) { rp2040.reboot(); }

/** @brief System clock. @return Hertz. */
static inline uint32_t eyesCpuHz(void) { return F_CPU; }

/**
 * @brief Is a button asking for USB drive mode?
 *
 * A pin of -1 uses BOOTSEL, which needs no extra hardware but halts XIP
 * briefly to sample the QSPI CS pin.
 *
 * @param pin GPIO to read, or -1 for BOOTSEL.
 * @return true if drive mode is requested.
 */
static inline bool eyesSafeModeRequested(int pin) {
  if (pin >= 0) {
    pinMode(pin, INPUT_PULLUP);
    delay(1);
    return digitalRead(pin) == LOW;
  }
  return BOOTSEL;
}

/** Pin eyesSafeModeRequested() reads when the sketch has not said. */
#define EYES_SAFE_MODE_PIN_DEFAULT -1

// ===========================================================================
#elif defined(ARDUINO_ARCH_ESP32)
// ===========================================================================

#define EYES_PLATFORM_NAME "ESP32" ///< Chip family, for the startup banner
#include <esp_heap_caps.h>

/** @brief Free heap in bytes. @return Bytes free. */
static inline uint32_t eyesFreeHeap(void) { return ESP.getFreeHeap(); }

/**
 * @brief Allocate memory the render loop will touch per pixel.
 *
 * PSRAM IS A TRAP FOR THIS WORKLOAD. With PSRAM configured, the default malloc
 * sends large blocks to external RAM -- and the polar maps and iris texture are
 * exactly that size. The render loop then does a random external read per
 * pixel, which costs far more than the arithmetic around it.
 *
 * So eye data is allocated MALLOC_CAP_INTERNAL, and the texture budget is
 * measured against internal RAM only. If internal RAM runs out the texture
 * loader simply decimates further, which costs sharpness rather than speed.
 *
 * @param n Bytes to allocate.
 * @return Pointer, or NULL.
 */
static inline void *eyesMalloc(size_t n) {
  void *p = heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  return p ? p : malloc(n); // Fall back rather than fail outright
}

/** @brief Largest internal-RAM allocation available. @return Bytes. */
static inline uint32_t eyesLargestFreeBlock(void) {
  return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL |
                                          MALLOC_CAP_8BIT);
}

/** @brief Restart the board. */
static inline void eyesReboot(void) { ESP.restart(); }

/** @brief System clock. @return Hertz. */
static inline uint32_t eyesCpuHz(void) {
  return getCpuFrequencyMhz() * 1000000UL;
}

/**
 * @brief Is a button asking for USB drive mode?
 * @param pin GPIO to read; most ESP32 boards wire BOOT to GPIO 0.
 * @return true if drive mode is requested.
 */
static inline bool eyesSafeModeRequested(int pin) {
  if (pin < 0)
    return false;
  pinMode(pin, INPUT_PULLUP);
  delay(1);
  return digitalRead(pin) == LOW;
}

/** Pin eyesSafeModeRequested() reads when the sketch has not said. */
#define EYES_SAFE_MODE_PIN_DEFAULT 0

// ===========================================================================
#else
// ===========================================================================
#error "Unsupported architecture -- add a block to Eyes_Platform.h"
#endif

#endif // _EYES_PLATFORM_H_
