/**
 * @file flash_config.h
 * @brief Which flash region holds the asset filesystem, per chip.
 */
// Flash transport selection, adapted from Adafruit's flash_config.h.
//
// Assets live on a FAT volume in the board's flash, read through
// Adafruit_SPIFlash + SdFat. Which region that is depends on the chip.

#ifndef FLASH_CONFIG_H_
#define FLASH_CONFIG_H_

#include <Adafruit_SPIFlash.h>

// ---------------------------------------------------------------------------
#if defined(ARDUINO_ARCH_RP2040) // Also RP2350 under arduino-pico
// ---------------------------------------------------------------------------
// The RP2 QSPI flash holds both the program and the filesystem, and the two
// schemes place the filesystem differently:
//
//   Adafruit_FlashTransport_RP2040       the partition set by
//                                        Tools > Flash Size (END of flash)
//   Adafruit_FlashTransport_RP2040_CPY   CircuitPython's layout
//                                        (start 1 MB, size = total - 1 MB)
//
// CPY is the default: it gives the familiar pre-formatted CIRCUITPY drive and
// matches how M4SK eye folders are laid out. With it, set Tools > Flash Size
// to an "FS 0MB" option so the core does not also claim the end of flash and
// overlap. Keep the sketch under 1 MB so it cannot collide with the start.

#  define USE_CIRCUITPY_PARTITION 1

#  if defined(USE_CIRCUITPY_PARTITION)
Adafruit_FlashTransport_RP2040_CPY flashTransport;
#  else
Adafruit_FlashTransport_RP2040 flashTransport;
#  endif

// ---------------------------------------------------------------------------
#elif defined(ARDUINO_ARCH_ESP32)
// ---------------------------------------------------------------------------
// The ESP32 keeps its filesystem in a FAT partition of the same flash as the
// program. This transport locates it by parsing the partition table, so
// Tools > Partition Scheme MUST include a FATFS partition -- for example
// "Default 4MB with ffat" or "8M with spiffs" replaced by a ffat variant.
// Without one, flash.begin() fails and the eye falls back to built-in
// defaults (a solid-colour eye), which is the symptom to look for.

Adafruit_FlashTransport_ESP32 flashTransport;

// ---------------------------------------------------------------------------
#elif defined(EXTERNAL_FLASH_USE_QSPI)
// ---------------------------------------------------------------------------
Adafruit_FlashTransport_QSPI flashTransport;

#elif defined(EXTERNAL_FLASH_USE_SPI)
Adafruit_FlashTransport_SPI flashTransport(EXTERNAL_FLASH_USE_CS,
                                           EXTERNAL_FLASH_USE_SPI);

#else
#  error "No flash transport for this board -- add a branch to flash_config.h"
#endif

#endif // FLASH_CONFIG_H_
