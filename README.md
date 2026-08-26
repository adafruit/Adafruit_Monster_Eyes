# Adafruit Monster Eyes [![Build Status](https://github.com/adafruit/Adafruit_Monster_Eyes/workflows/Arduino%20Library%20CI/badge.svg)](https://github.com/adafruit/Adafruit_Monster_Eyes/actions)[![Documentation](https://github.com/adafruit/ci-arduino/blob/master/assets/doxygen_badge.svg)](http://adafruit.github.io/Adafruit_Monster_Eyes/html/index.html)

Arduino code to show moving, blinking eyes on various displays. Ported from the M4_Eyes for the MONSTER M4SK by Phil B. for Adafruit Industries

## MCU and Display Support with Expected Performance

The following microcontrollerss have been tested:

* RP2040
* RP2350
* ESP32-S2
* ESP32-S3

TFT support currently includes:
* ST7789
* GC9A01A

DVI output is only available on RP2 (PicoDVI).

Expected performance (avg):

| MCU:Display    |  1 eye  |  2 eyes |
| :------------- | :-----: | ------: |
| RP2040 : TFT   | 24 FPS  | 13 FPS  |
| RP2350 : TFT   | 37 FPS  | 19 FPS  |
| RP2040 : DVI   | 180 FPS | 100 FPS |
| ESP32-S2 : TFT | 32 FPS  | 16 FPS  |
| ESP32-S3 : TFT | 39 FPS  | 20 FPS  |

On average, adding a second eye will halve the frame rate seen while showing one eye.

## Build Process

The repository has pre-built UF2 and .BIN files for common board/display combinations.

If building on your own, edit the settings.h file to update pin, display and eye count defines.

The default pin mapping is:
SCK - SCK (default mapping in Arduino BSP)
MISO - MISO (default mapping in Arduino BSP)
DC - 9
RST - 10
CS (display 0) - 11
CS (display 1) - 12

These pins have compatibility across Metro and Feather form factor boards.
