// Flash transport selection, adapted from Adafruit's flash_config.h.
//
// PARTITION CHOICE MATTERS. The RP2040's QSPI flash holds both the program
// and the filesystem, and the two schemes place the filesystem differently:
//
//   Adafruit_FlashTransport_RP2040       uses the partition defined by
//                                        Tools > Flash Size (at the END of
//                                        flash)
//   Adafruit_FlashTransport_RP2040_CPY   uses CircuitPython's layout
//                                        (start 1 MB, size = total - 1 MB)
//
// The CPY layout is the default here, because it gives you the familiar
// pre-formatted CIRCUITPY drive and matches how M4SK eye projects are laid
// out. With it, set Tools > Flash Size to a "FS 0MB" option so the core does
// not also claim a region at the end of flash and overlap.
//
// Keep the sketch under 1 MB (this one is a few hundred KB) so it cannot
// collide with the filesystem start.

#ifndef FLASH_CONFIG_H_
#define FLASH_CONFIG_H_

#include <Adafruit_SPIFlash.h>

// Comment this out to use the arduino-pico Tools > Flash Size partition
// instead of the CircuitPython one.
#define USE_CIRCUITPY_PARTITION 1

#if !defined(ARDUINO_ARCH_RP2040)
  #error "This build targets RP2040 / RP2350."
#endif

#if defined(USE_CIRCUITPY_PARTITION)
  Adafruit_FlashTransport_RP2040_CPY flashTransport;
#else
  // (start=0, size=0) means "match the Tools > Flash Size selection"
  Adafruit_FlashTransport_RP2040 flashTransport;
#endif

#endif  // FLASH_CONFIG_H_
