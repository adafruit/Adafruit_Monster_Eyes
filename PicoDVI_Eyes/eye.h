// M4_Eyes on PicoDVI -- single header for configuration, types and interfaces.
//
// Everything the eye needs at runtime comes from a FAT volume in the RP2040's
// QSPI flash, read with Adafruit_SPIFlash + SdFat. By default that is the
// CircuitPython partition, so the drive is the usual CIRCUITPY volume and
// M4SK-style eye folders drop straight in. See flash_config.h to change it.
//
// There are no baked-in assets: if a file is missing or unreadable the eye
// degrades the way the original M4_Eyes did -- a missing texture becomes a
// solid color, a missing eyelid becomes no eyelid -- so the sketch always
// renders something.

#pragma once
#include <stdint.h>
#include <stddef.h>

// ===========================================================================
// CONFIGURATION -- defaults only; config.eye on the drive overrides these
// ===========================================================================

#define FB_WIDTH        320
#define FB_HEIGHT       240
#define NUM_EYES          1     // Single centered eye

// Change to match your carrier:
//   adafruit_feather_dvi_cfg  Feather RP2040 DVI
//   adafruit_dvibell_cfg      PiCowbell DVI
//   pico_sock_cfg             Pico DVI Sock
//   pimoroni_demo_hdmi_cfg    Pimoroni Pico DV
#define DVI_PIN_CONFIG  adafruit_feather_dvi_cfg

// Path to the config file on the drive. Assets it references are resolved
// relative to the volume root, e.g. "/demon/iris.bmp".
#define CONFIG_FILENAME "/config.eye"

// Which per-eye override block to read from a two-eye .eye file.
// "left" reproduces the eye that appeared on the right of the original pair.
#define EYE_SIDE        "left"

// RAM BUDGET (RP2040 has 264 KB). The 16-bit framebuffer is a fixed 153,600
// bytes and the polar maps grow as mapRadius^2, so displaySize is the knob:
//
//   size  maps+displace  +framebuffer   left for textures
//   144       47234         200834          ~41 KB
//   160       57600         211200          ~31 KB
//   176       68994         222594          ~20 KB
//   192       81416         235016           ~7 KB
//
// If a config asks for more than fits, setup() steps displaySize down by 16
// until it does. Textures are decimated to fit whatever remains.
#define DISPLAY_SIZE      176

// Heap kept clear of textures for stack, TinyUSB and libdvi. Raise if you see
// allocation failures; lower if textures come out smaller than you'd like.
// Serial prints the real free-heap numbers during startup.
#define HEAP_RESERVE    6000

#define EYE_RADIUS         93   // 0 = derive as displaySize/2 + 5
#define IRIS_RADIUS        80
#define SLIT_PUPIL_RADIUS  71   // 0 = round pupil; smaller = narrower slit

// mapRadius = eyeRadius * pi * coverage. Don't go far below 0.55: the saccade
// radius is (mapDiameter - displaySize*pi/2) * 0.75, which collapses to zero
// as coverage shrinks, freezing the gaze.
#define COVERAGE        0.6f

// Native-endian RGB565. The big-endian convention the SAMD51 SPI-DMA path
// needed is gone, because GFXcanvas16 stores native uint16_t.
#define PUPIL_COLOR     0x0000
#define BACK_COLOR      0x5000  // (80, 0, 0)
#define EYELID_COLOR    0x0000
#define IRIS_COLOR      0x001F  // Used when no iris texture is present
#define SCLERA_COLOR    0xFFFF  // Used when no sclera texture is present

#define PUPIL_MIN       0.05f
#define PUPIL_MAX       0.25f
#define TRACKING           0
#define TRACK_FACTOR    0.5f
#define GAZE_MAX     3000000    // Max microseconds between major eye movements

#define IRIS_SPIN      -18.0f   // RPM
#define IRIS_START_ANGLE  512   // 0-1023 CCW
#define EYELID_MIRROR       1

// ===========================================================================
// SETTINGS
// ===========================================================================

#define EYE_PATH_MAX 64

struct EyeSettings {
  int      displaySize, eyeRadius, irisRadius, slitPupilRadius;
  float    coverage;
  uint16_t pupilColor, backColor, eyelidColor, irisColor, scleraColor;
  float    pupilMin, pupilMax;
  bool     tracking;
  float    trackFactor;
  uint32_t gazeMax;
  float    irisSpin, scleraSpin;        // RPM, positive = clockwise to viewer
  uint16_t irisStartAngle, scleraStartAngle;
  uint16_t irisMirror, scleraMirror;    // 0 or 1023
  bool     eyelidMirror;
  char     irisFile[EYE_PATH_MAX], scleraFile[EYE_PATH_MAX];
  char     upperFile[EYE_PATH_MAX], lowerFile[EYE_PATH_MAX];
};

extern EyeSettings settings;

void eyeSettingsDefaults(void);
bool eyeSettingsLoad(const char *filename);
void eyeSettingsFinalize(void);

// ===========================================================================
// STORAGE
// ===========================================================================

bool eyeStorageBegin(void);
void eyeStorageEnd(void);
bool eyeStorageDriveModeRequested(void);
void eyeStorageRunDriveMode(void);   // Never returns; reboots on eject

// ===========================================================================
// TABLES
// ===========================================================================

extern uint8_t *displace;    // (size/2)^2, 255 = outside eyeball
extern uint8_t *polarAngle;  // mapRadius^2
extern int8_t  *polarDist;   // mapRadius^2, >=0 sclera, <0 iris, -128 = off
extern int      mapRadius, mapDiameter;

bool  eyeTablesInit(void);
void  eyeTablesFree(void);
float screen2map(int in);
float map2screen(int in);

// ===========================================================================
// MEDIA
// ===========================================================================

extern uint8_t *upperOpen, *upperClosed, *lowerOpen, *lowerClosed;
extern const uint16_t *irisData, *scleraData;

uint16_t irisWidth(void),   irisHeight(void);
uint16_t scleraWidth(void), scleraHeight(void);

bool eyeMediaLoad(int size, uint32_t texBudget);

// ===========================================================================
// BMP
// ===========================================================================

class BmpReader {
public:
  virtual ~BmpReader() {}
  virtual bool   seek(uint32_t pos) = 0;
  virtual size_t read(void *buf, size_t len) = 0;
};

bool bmpLoadEyelid(BmpReader &r, uint8_t *openTable, uint8_t *closedTable,
                   int size, bool isUpper);
bool bmpLoadTexture(BmpReader &r, uint16_t **data, uint16_t *width,
                    uint16_t *height, uint32_t maxBytes);
