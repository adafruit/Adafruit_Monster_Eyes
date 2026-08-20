// M4_Eyes on PicoDVI -- support code.
//
// Sections, in order:
//   1. BMP loading      (streaming, 1-bit eyelids and 24-bit textures)
//   2. Settings         (config.eye JSON)
//   3. Storage          (FatFS mount and USB drive mode)
//   4. Tables           (polar and displacement maps)
//   5. Media            (ties files to the renderer, with solid-color fallback)

// Requires ArduinoJson 7.x (JsonDocument). On 6.x use StaticJsonDocument<2048>.
#define ARDUINOJSON_ENABLE_COMMENTS 1
#include <ArduinoJson.h>
#include <Arduino.h>
#include <SPI.h>
#include "SdFat_Adafruit_Fork.h"
#include <Adafruit_SPIFlash.h>
#include <Adafruit_TinyUSB.h>
#include "flash_config.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "eye.h"

// ===========================================================================
// 1. BMP LOADING
// ===========================================================================
//
// Replaces Adafruit_ImageReader (and its SdFat / Adafruit_SPIFlash
// dependencies) with the only two cases the eye needs. Both loaders STREAM --
// nothing allocates a full-size intermediate copy, which is what forced the
// "booster seat" fragmentation workaround in the original M4_Eyes. The texture
// loader also decimates on the fly to fit the RAM budget it is handed, so an
// oversized BMP loses resolution instead of failing.
//
// Output is native-endian RGB565 to match GFXcanvas16.

// Angular resolution past 512 is wasted: the renderer indexes as
// (angle * width / 1024) with angle 0-1023, and distance is 0-127.
#define TEX_MAX_W 512
#define TEX_MAX_H 128

struct BmpInfo {
  int32_t  width, height;   // height always positive; see topDown
  uint16_t bpp;
  uint32_t dataOffset, rowSize;
  bool     topDown;
  uint8_t  whiteIndex;      // 1-bit only: the lighter palette entry
};

static uint16_t rd16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint32_t rd32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static bool bmpReadHeader(BmpReader &r, BmpInfo &info) {
  uint8_t hdr[54];
  if (!r.seek(0)) return false;
  if (r.read(hdr, sizeof(hdr)) != sizeof(hdr)) return false;
  if ((hdr[0] != 'B') || (hdr[1] != 'M')) return false;

  info.dataOffset      = rd32(&hdr[10]);
  uint32_t dibSize     = rd32(&hdr[14]);
  int32_t  w           = (int32_t)rd32(&hdr[18]);
  int32_t  h           = (int32_t)rd32(&hdr[22]);
  info.bpp             = rd16(&hdr[28]);
  uint32_t compression = rd32(&hdr[30]);

  if (dibSize < 40) return false;                        // BITMAPINFOHEADER+
  if (compression != 0) return false;                    // BI_RGB only
  if ((info.bpp != 1) && (info.bpp != 24)) return false;
  if (w <= 0) return false;

  info.topDown = (h < 0);          // Negative height means top-down rows
  info.height  = info.topDown ? -h : h;
  info.width   = w;
  if (info.height <= 0) return false;

  info.rowSize = (((uint32_t)w * info.bpp + 31) / 32) * 4;  // 4-byte padded

  info.whiteIndex = 1;
  if (info.bpp == 1) {
    uint8_t pal[8];                // 2 entries, each B,G,R,reserved
    if (!r.seek(14 + dibSize)) return false;
    if (r.read(pal, sizeof(pal)) != sizeof(pal)) return false;
    int lum0 = pal[0] + pal[1] + pal[2];
    int lum1 = pal[4] + pal[5] + pal[6];
    info.whiteIndex = (lum1 > lum0) ? 1 : 0;
  }
  return true;
}

// Mirrors loadEyelid() in the original file.cpp: per column, find the topmost
// and bottommost lit pixel, then flip into render space where +Y is up.
// Unlike the original, which centered and CLIPPED against a fixed 240px
// screen, this scales proportionally so any source size fits any eye size.
bool bmpLoadEyelid(BmpReader &r, uint8_t *openTable, uint8_t *closedTable,
                   int size, bool isUpper) {
  BmpInfo info;
  if (!bmpReadHeader(r, info)) return false;
  if (info.bpp != 1) return false;
  if (info.height < 2) return false;

  const uint8_t init = isUpper ? (uint8_t)(size - 1) : 0;
  memset(openTable,   init, size);
  memset(closedTable, init, size);

  uint16_t *minRow = (uint16_t *)malloc((size_t)size * 2 * sizeof(uint16_t));
  if (!minRow) return false;
  uint16_t *maxRow = &minRow[size];
  for (int i = 0; i < size; i++) { minRow[i] = 0xFFFF; maxRow[i] = 0; }

  uint8_t *row = (uint8_t *)malloc(info.rowSize);
  if (!row) { free(minRow); return false; }

  bool ok = true;
  for (int32_t fileRow = 0; fileRow < info.height; fileRow++) {
    if (!r.seek(info.dataOffset + (uint32_t)fileRow * info.rowSize) ||
        (r.read(row, info.rowSize) != info.rowSize)) { ok = false; break; }

    // Bottom-up is the BMP default: file row 0 is the image's last row
    int32_t imageRow = info.topDown ? fileRow : (info.height - 1 - fileRow);

    for (int32_t sx = 0; sx < info.width; sx++) {
      uint8_t bit = (row[sx >> 3] >> (7 - (sx & 7))) & 1;
      if (bit != info.whiteIndex) continue;
      int dx = (int)((int64_t)sx * size / info.width);
      if (dx < 0) dx = 0; else if (dx >= size) dx = size - 1;
      if ((uint16_t)imageRow < minRow[dx]) minRow[dx] = (uint16_t)imageRow;
      if ((uint16_t)imageRow > maxRow[dx]) maxRow[dx] = (uint16_t)imageRow;
    }
  }
  free(row);

  if (ok) {
    for (int dx = 0; dx < size; dx++) {
      if (minRow[dx] == 0xFFFF) continue;    // No data; keep the init value
      int my = (int)((int64_t)minRow[dx] * (size - 1) / (info.height - 1));
      int My = (int)((int64_t)maxRow[dx] * (size - 1) / (info.height - 1));
      if (my < 0) my = 0; else if (my > size - 1) my = size - 1;
      if (My < 0) My = 0; else if (My > size - 1) My = size - 1;
      if (isUpper) {
        openTable[dx]   = (uint8_t)(size - 1 - my);
        closedTable[dx] = (uint8_t)(size - 1 - My);
      } else {
        closedTable[dx] = (uint8_t)(size - 1 - my);
        openTable[dx]   = (uint8_t)(size - 1 - My);
      }
    }
  }
  free(minRow);
  return ok;
}

bool bmpLoadTexture(BmpReader &r, uint16_t **data, uint16_t *width,
                    uint16_t *height, uint32_t maxBytes) {
  BmpInfo info;
  if (!bmpReadHeader(r, info)) return false;
  if (info.bpp != 24) return false;

  // Start at the source size capped to what the renderer can address, then
  // shrink the longer dimension until it fits the budget.
  int dw = (int)info.width  < TEX_MAX_W ? (int)info.width  : TEX_MAX_W;
  int dh = (int)info.height < TEX_MAX_H ? (int)info.height : TEX_MAX_H;
  while (((uint32_t)dw * dh * 2 > maxBytes) && ((dw > 8) || (dh > 4))) {
    if ((dw * (int)info.height) > (dh * (int)info.width)) {
      if (dw > 8) dw--; else dh--;
    } else {
      if (dh > 4) dh--; else dw--;
    }
  }
  if ((uint32_t)dw * dh * 2 > maxBytes) return false;

  uint16_t *dst = (uint16_t *)malloc((size_t)dw * dh * 2);
  if (!dst) return false;
  uint8_t *row = (uint8_t *)malloc(info.rowSize);
  if (!row) { free(dst); return false; }

  bool ok = true;
  for (int dy = 0; dy < dh; dy++) {
    int32_t imageRow = (int32_t)((int64_t)dy * info.height / dh);
    int32_t fileRow  = info.topDown ? imageRow : (info.height - 1 - imageRow);
    if (!r.seek(info.dataOffset + (uint32_t)fileRow * info.rowSize) ||
        (r.read(row, info.rowSize) != info.rowSize)) { ok = false; break; }

    uint16_t *out = &dst[(size_t)dy * dw];
    for (int dx = 0; dx < dw; dx++) {
      int32_t sx = (int32_t)((int64_t)dx * info.width / dw);
      const uint8_t *p = &row[(size_t)sx * 3];    // Stored B, G, R
      *out++ = (uint16_t)(((p[2] & 0xF8) << 8) | ((p[1] & 0xFC) << 3) | (p[0] >> 3));
    }
  }
  free(row);
  if (!ok) { free(dst); return false; }

  *data = dst; *width = (uint16_t)dw; *height = (uint16_t)dh;
  return true;
}

// ===========================================================================
// 2. SETTINGS
// ===========================================================================

// The QSPI flash chip and the FAT volume living on it. Both are shared by the
// settings loader, the media loader and drive mode.
Adafruit_SPIFlash flash(&flashTransport);
FatVolume         fatfs;
static bool       fsMounted = false;

EyeSettings settings;

void eyeSettingsDefaults(void) {
  memset(&settings, 0, sizeof(settings));
  settings.displaySize      = DISPLAY_SIZE;
  settings.eyeRadius        = EYE_RADIUS;
  settings.irisRadius       = IRIS_RADIUS;
  settings.slitPupilRadius  = SLIT_PUPIL_RADIUS;
  settings.coverage         = COVERAGE;
  settings.pupilColor       = PUPIL_COLOR;
  settings.backColor        = BACK_COLOR;
  settings.eyelidColor      = EYELID_COLOR;
  settings.irisColor        = IRIS_COLOR;
  settings.scleraColor      = SCLERA_COLOR;
  settings.pupilMin         = PUPIL_MIN;
  settings.pupilMax         = PUPIL_MAX;
  settings.tracking         = TRACKING;
  settings.trackFactor      = TRACK_FACTOR;
  settings.gazeMax          = GAZE_MAX;
  settings.irisSpin         = IRIS_SPIN;
  settings.scleraSpin       = 0.0f;
  settings.irisStartAngle   = IRIS_START_ANGLE;
  settings.scleraStartAngle = IRIS_START_ANGLE;
  settings.eyelidMirror     = EYELID_MIRROR;
}

// "Do What I Mean" decoder from the original file.cpp. Accepts 42, "0x2A",
// "0xF800", [255,0,0], ["0xFF",0,0], [1.0,0.0,0.0]. Unlike the original this
// returns NATIVE-endian RGB565.
static int32_t dwim(JsonVariantConst v, int32_t def = 0) {
  if (v.is<int>()) {
    return v.as<int>();
  } else if (v.is<float>()) {
    return (int32_t)(v.as<float>() + 0.5f);
  } else if (v.is<const char *>()) {
    return (int32_t)strtol(v.as<const char *>(), NULL, 0);
  } else if (v.is<JsonArrayConst>()) {
    JsonArrayConst a = v.as<JsonArrayConst>();
    if (a.size() >= 3) {
      long cc[3];
      for (uint8_t i = 0; i < 3; i++) {
        if (a[i].is<int>())               cc[i] = a[i].as<int>();
        else if (a[i].is<float>())        cc[i] = (long)(a[i].as<float>() * 255.999f);
        else if (a[i].is<const char *>()) cc[i] = strtol(a[i].as<const char *>(), NULL, 0);
        else                              cc[i] = 0;
        if (cc[i] > 255) cc[i] = 255; else if (cc[i] < 0) cc[i] = 0;
      }
      return ((cc[0] & 0xF8) << 8) | ((cc[1] & 0xFC) << 3) | (cc[2] >> 3);
    }
    if (a.size() >= 1) {
      if (a[0].is<int>()) return a[0].as<int>();
      return strtol(a[0].as<const char *>(), NULL, 0);
    }
  }
  return def;
}

static void copyStr(char *dst, JsonVariantConst v) {
  if (v.is<const char *>()) {
    strncpy(dst, v.as<const char *>(), EYE_PATH_MAX - 1);
    dst[EYE_PATH_MAX - 1] = 0;
  }
}

// Apply one JSON object: the document root, or a per-eye sub-object on top.
static void applyObject(JsonVariantConst o) {
  if (o.isNull()) return;
  JsonVariantConst v;

  settings.displaySize     = dwim(o["displaySize"],     settings.displaySize);
  settings.eyeRadius       = dwim(o["eyeRadius"],       settings.eyeRadius);
  settings.irisRadius      = dwim(o["irisRadius"],      settings.irisRadius);
  settings.slitPupilRadius = dwim(o["slitPupilRadius"], settings.slitPupilRadius);
  settings.gazeMax         = (uint32_t)dwim(o["gazeMax"], (int32_t)settings.gazeMax);

  v = o["coverage"];
  if (v.is<float>() || v.is<int>()) settings.coverage = v.as<float>();

  settings.pupilColor  = (uint16_t)dwim(o["pupilColor"],  settings.pupilColor);
  settings.backColor   = (uint16_t)dwim(o["backColor"],   settings.backColor);
  settings.irisColor   = (uint16_t)dwim(o["irisColor"],   settings.irisColor);
  settings.scleraColor = (uint16_t)dwim(o["scleraColor"], settings.scleraColor);

  // Legacy eyelidIndex expands to a gray via index * 0x0101, which is
  // byte-symmetric and so survives the endianness change untouched. A full
  // 16-bit eyelidColor is also accepted now that the byte-repeat trick the
  // SPI-DMA path relied on is gone.
  v = o["eyelidIndex"];
  if (!v.isNull()) settings.eyelidColor = (uint16_t)(dwim(v) & 0xFF) * 0x0101;
  v = o["eyelidColor"];
  if (!v.isNull()) settings.eyelidColor = (uint16_t)dwim(v, settings.eyelidColor);

  v = o["pupilMin"]; if (v.is<float>() || v.is<int>()) settings.pupilMin = v.as<float>();
  v = o["pupilMax"]; if (v.is<float>() || v.is<int>()) settings.pupilMax = v.as<float>();
  v = o["tracking"]; if (v.is<bool>()) settings.tracking = v.as<bool>();
  v = o["squint"];
  if (v.is<float>() || v.is<int>()) settings.trackFactor = 1.0f - v.as<float>();

  v = o["irisSpin"];   if (v.is<float>() || v.is<int>()) settings.irisSpin   = v.as<float>();
  v = o["scleraSpin"]; if (v.is<float>() || v.is<int>()) settings.scleraSpin = v.as<float>();

  v = o["irisAngle"];
  if (v.is<int>())        settings.irisStartAngle = 1023 - (v.as<int>() & 1023);
  else if (v.is<float>()) settings.irisStartAngle = 1023 - ((int)(v.as<float>() * 1024.0f) & 1023);
  v = o["scleraAngle"];
  if (v.is<int>())        settings.scleraStartAngle = 1023 - (v.as<int>() & 1023);
  else if (v.is<float>()) settings.scleraStartAngle = 1023 - ((int)(v.as<float>() * 1024.0f) & 1023);

  v = o["irisMirror"];   if (v.is<bool>() || v.is<int>()) settings.irisMirror   = v.as<bool>() ? 1023 : 0;
  v = o["scleraMirror"]; if (v.is<bool>() || v.is<int>()) settings.scleraMirror = v.as<bool>() ? 1023 : 0;
  v = o["eyelidMirror"]; if (v.is<bool>() || v.is<int>()) settings.eyelidMirror = v.as<bool>();

  copyStr(settings.irisFile,   o["irisTexture"]);
  copyStr(settings.scleraFile, o["scleraTexture"]);
  copyStr(settings.upperFile,  o["upperEyelid"]);
  copyStr(settings.lowerFile,  o["lowerEyelid"]);
}

bool eyeSettingsLoad(const char *filename) {
  if (!fsMounted) return false;
  File32 f = fatfs.open(filename, FILE_READ);
  if (!f) {
    Serial.printf("No %s on drive; using built-in defaults\n", filename);
    return false;
  }
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) {
    Serial.printf("Config parse error (%s); using built-in defaults\n", err.c_str());
    return false;
  }
  applyObject(doc.as<JsonVariantConst>());
  applyObject(doc[EYE_SIDE].as<JsonVariantConst>());
  Serial.printf("Loaded %s\n", filename);
  return true;
}

void eyeSettingsFinalize(void) {
  if (settings.displaySize < 64)  settings.displaySize = 64;
  if (settings.displaySize > 240) settings.displaySize = 240;
  settings.displaySize &= ~1;          // Keep even; the renderer halves it

  if (settings.eyeRadius <= 0) settings.eyeRadius = settings.displaySize / 2 + 5;
  else                         settings.eyeRadius = abs(settings.eyeRadius);

  if (settings.irisRadius <= 0) settings.irisRadius = settings.displaySize / 4;
  else                          settings.irisRadius = abs(settings.irisRadius);
  // screen2map() takes sqrt(eyeRadius^2 - irisRadius^2); keep it real
  if (settings.irisRadius >= settings.eyeRadius)
    settings.irisRadius = settings.eyeRadius - 1;

  settings.slitPupilRadius = abs(settings.slitPupilRadius);
  if (settings.slitPupilRadius > settings.irisRadius)
    settings.slitPupilRadius = settings.irisRadius;

  if (settings.coverage < 0.0f) settings.coverage = 0.0f;
  else if (settings.coverage > 1.0f) settings.coverage = 1.0f;

  if (settings.pupilMin < 0.0f) settings.pupilMin = 0.0f;
  if (settings.pupilMax > 1.0f) settings.pupilMax = 1.0f;
  if (settings.pupilMin > settings.pupilMax) {
    float t = settings.pupilMin;
    settings.pupilMin = settings.pupilMax;
    settings.pupilMax = t;
  }
  if (settings.trackFactor < 0.0f) settings.trackFactor = 0.0f;
  else if (settings.trackFactor > 1.0f) settings.trackFactor = 1.0f;
}

// ===========================================================================
// 3. STORAGE
// ===========================================================================
//
// Assets live on a FAT volume in the RP2040's QSPI flash, read through
// Adafruit_SPIFlash + SdFat. By default this is the CircuitPython partition
// (see flash_config.h), so the drive is the familiar pre-formatted CIRCUITPY
// volume and M4SK-style eye folders drop straight in.
//
// WHY DRIVE MODE IS A SEPARATE BOOT MODE RATHER THAN CONCURRENT:
//
//   Reading is safe. Adafruit_FlashTransport_RP2040 reads through the
//   memory-mapped XIP window, which costs nothing and does not disturb video.
//
//   Writing is not. Erase and program must disable XIP, which means any code
//   or data fetched from flash during that window returns garbage. PicoDVI
//   owns core1 and both PIO blocks and runs continuously, so a host write
//   landing mid-frame risks a hang. A 4 KB sector erase alone is tens of
//   milliseconds.
//
//   FAT also wants exclusive access: if the host and the sketch both write,
//   the volume corrupts.
//
// So the eye reads its files once at startup and never writes. To change
// files, hold BOOTSEL at reset: DVI never starts, the drive is exported
// read-write over USB, and the board reboots once writing goes quiet.

static Adafruit_USBD_MSC usb_msc;
static volatile bool     mscWritten     = false;
static volatile uint32_t lastWriteMillis = 0;

// These three run in USB interrupt context. Keep them to block I/O only.
static int32_t mscReadCb(uint32_t lba, void *buffer, uint32_t bufsize) {
  return flash.readBlocks(lba, (uint8_t *)buffer, bufsize / 512) ? (int32_t)bufsize : -1;
}

static int32_t mscWriteCb(uint32_t lba, uint8_t *buffer, uint32_t bufsize) {
  mscWritten = true;
  lastWriteMillis = millis();
  return flash.writeBlocks(lba, buffer, bufsize / 512) ? (int32_t)bufsize : -1;
}

static void mscFlushCb(void) {
  flash.syncBlocks();
  fatfs.cacheClear();     // Our cached view is stale after a host write
  lastWriteMillis = millis();
}

bool eyeStorageBegin(void) {
  if (!flash.begin()) {
    Serial.println("Flash chip init failed.");
    fsMounted = false;
    return false;
  }
  Serial.printf("Flash JEDEC ID 0x%06lX, %lu bytes\n",
                (unsigned long)flash.getJEDECID(), (unsigned long)flash.size());

  if (!fatfs.begin(&flash)) {
    Serial.println("No FAT filesystem found on the flash partition.");
    Serial.println("Either load CircuitPython once to create CIRCUITPY, or");
    Serial.println("hold BOOTSEL at reset and let the host format the drive.");
    fsMounted = false;
    return false;
  }
  fsMounted = true;
  return true;
}

void eyeStorageEnd(void) {
  // Nothing to unmount in the SdFat sense; we simply stop reading. Flash is
  // never written in eye mode, so leaving the volume mounted is harmless.
  fsMounted = false;
}

bool eyeStorageDriveModeRequested(void) {
  // arduino-pico exposes BOOTSEL as a pseudo-pin. Reading it briefly halts
  // XIP, which is harmless here because this runs before DVI starts.
  return BOOTSEL;
}

void eyeStorageRunDriveMode(void) {
  Serial.println("=== USB DRIVE MODE ===");
  Serial.println("DVI is intentionally off. Copy files, then eject.");

  if (!flash.begin()) {
    Serial.println("Flash chip init failed; cannot export a drive.");
    for (;;) delay(1000);
  }
  // Mount if we can. An unformatted volume is still exported, so the host can
  // format it, which is the recovery path when CircuitPython was never loaded.
  fsMounted = fatfs.begin(&flash);
  if (!fsMounted) Serial.println("Volume not mountable -- format it from the host.");

  usb_msc.setID("Adafruit", "Eye Assets", "1.0");
  usb_msc.setCapacity(flash.size() / 512, 512);
  usb_msc.setReadWriteCallback(mscReadCb, mscWriteCb, mscFlushCb);
  usb_msc.setUnitReady(true);
  usb_msc.begin();

  Serial.println("Drive exported. Rebooting automatically once writes stop.");

#ifdef LED_BUILTIN
  pinMode(LED_BUILTIN, OUTPUT);
#endif
  uint32_t lastBlink = 0;
  bool     ledState  = false;

  for (;;) {
    // There is no reliable "ejected" signal, so use quiet time instead: once
    // the host has written something and then stayed silent for a couple of
    // seconds, the copy is finished and it is safe to restart.
    if (mscWritten && ((millis() - lastWriteMillis) > 2000)) {
      Serial.println("Writes finished -- rebooting into eye mode.");
      flash.syncBlocks();
      delay(250);
      rp2040.reboot();
    }
    uint32_t now = millis();
    uint32_t period = mscWritten ? 120 : 600;   // Fast blink after a write
    if ((now - lastBlink) >= period) {
      lastBlink = now;
      ledState = !ledState;
#ifdef LED_BUILTIN
      digitalWrite(LED_BUILTIN, ledState);
#endif
    }
    delay(5);
  }
}

// ===========================================================================
// 4. TABLES
// ===========================================================================
//
// Adapted from tablegen.cpp. The math is unchanged; sizes come from settings.
//
// The round eyeball is faked with a 2D displacement map rather than real 3D
// rotation. Both tables cover ONE QUADRANT and are mirrored at render time.
//
// STARTUP COST: with a slit pupil, calcMap() runs a brute-force search per
// iris pixel. The RP2040 has no FPU, so expect a few seconds of blank screen.

uint8_t *displace    = NULL;
uint8_t *polarAngle  = NULL;
int8_t  *polarDist   = NULL;
int      mapRadius   = 0;
int      mapDiameter = 0;

float screen2map(int in) {
  return atan2f((float)in,
                sqrtf((float)(settings.eyeRadius * settings.eyeRadius - in * in))) /
         (float)M_PI_2 * (float)mapRadius;
}

float map2screen(int in) {
  return sinf((float)in / (float)mapRadius) * (float)M_PI_2 * (float)settings.eyeRadius;
}

static bool calcDisplacement(void) {
  const int half = settings.displaySize / 2;
  displace = (uint8_t *)malloc(half * half);
  if (!displace) return false;

  const float eyeRadius2 = (float)(settings.eyeRadius * settings.eyeRadius);
  uint8_t    *ptr        = displace;

  // First quadrant only, "+Y is up". Pixel centers at +0.5 by design; that
  // makes mirroring numerically correct.
  for (int y = 0; y < half; y++) {
    float dy = (float)y + 0.5f;
    dy *= dy;
    for (int x = 0; x < half; x++) {
      float dx = (float)x + 0.5f;
      float d2 = dx * dx + dy;
      if (d2 <= eyeRadius2) {
        float d  = sqrtf(d2);
        float h  = sqrtf(eyeRadius2 - d2);   // Hemisphere height at d
        float a  = atan2f(d, h);             // 0 to pi/2 from center
        float pa = a / (float)M_PI_2 * (float)mapRadius;
        dx      /= d;
        *ptr++   = (uint8_t)(dx * pa) - x;
      } else {
        *ptr++ = 255;                        // Outside the eye
      }
    }
  }
  return true;
}

static bool calcMap(void) {
  const int pixels = mapRadius * mapRadius;

  polarAngle = (uint8_t *)malloc(pixels * 2);   // One alloc for both tables
  if (!polarAngle) return false;
  polarDist = (int8_t *)&polarAngle[pixels];

  const float mapRadius2  = (float)mapRadius * (float)mapRadius;
  const float iRad        = screen2map(settings.irisRadius);
  const float irisRadius2 = iRad * iRad;

  uint8_t *anglePtr = polarAngle;
  int8_t  *distPtr  = polarDist;

  for (int y = 0; y < mapRadius; y++) {
    float dy = (float)y + 0.5f, dy2 = dy * dy;
    for (int x = 0; x < mapRadius; x++) {
      float dx = (float)x + 0.5f;
      float d2 = dx * dx + dy2;
      if (d2 > mapRadius2) {
        *anglePtr++ = 0;
        *distPtr++  = -128;
      } else {
        float angle = (float)M_PI_2 - atan2f(dy, dx);   // Clockwise, 0 at top
        angle *= 512.0f / (float)M_PI;                  // 0 to <256 in Q1
        *anglePtr++ = (uint8_t)angle;
        float d = sqrtf(d2);
        if (d2 > irisRadius2) {                         // Sclera: 0..127
          d = ((float)mapRadius - d) / ((float)mapRadius - iRad);
          *distPtr++ = (int8_t)(d * 127.0f);
        } else {                                        // Iris: -1..-127
          d = (iRad - d) / iRad;
          *distPtr++ = (int8_t)(d * -127.0f) - 1;
        }
      }
    }
  }

  if (settings.slitPupilRadius > 0) {
    for (int y = 0; y < mapRadius; y++) {
      float dy = (float)y + 0.5f, dy2 = dy * dy;
      for (int x = 0; x < mapRadius; x++) {
        float dx = (float)x + 0.5f;
        float d2 = dx * dx + dy2;
        if (d2 > irisRadius2) continue;
        float xp = (float)x + 0.5f;
        for (int i = 126; i >= 0; i--) {
          float ratio = (float)i / 128.0f;   // 0.0 open .. just under 1.0 slit
          // A point between top of iris and top of slit pupil, and another
          // between right of iris and center; find the circle through both.
          float y1 = iRad - (iRad - (float)settings.slitPupilRadius) * ratio;
          float x2 = iRad * (1.0f - ratio);
          float xc = (x2 * x2 - y1 * y1) / (2.0f * x2);
          float rx = x2 - xc;
          float px = xp - xc;
          if ((px * px + dy2) <= (rx * rx)) {
            polarDist[y * mapRadius + x] = (int8_t)(-1 - i);
            break;
          }
        }
      }
    }
  }
  return true;
}

void eyeTablesFree(void) {
  if (polarAngle) { free(polarAngle); polarAngle = NULL; polarDist = NULL; }
  if (displace)   { free(displace);   displace   = NULL; }
}

bool eyeTablesInit(void) {
  eyeTablesFree();
  mapRadius = (int)((float)settings.eyeRadius * (float)M_PI * settings.coverage + 0.5f);
  mapDiameter = mapRadius * 2;
  if (mapRadius < 8) return false;
  if (!calcMap())          { eyeTablesFree(); return false; }
  if (!calcDisplacement()) { eyeTablesFree(); return false; }
  return true;
}

// ===========================================================================
// 5. MEDIA
// ===========================================================================
//
// Everything is optional. A missing texture becomes a 1x1 buffer holding the
// solid color from settings, which the renderer samples correctly and which
// still produces a properly sized, dilating pupil. A missing eyelid leaves
// the sweep tables at their init values, which reads as "no eyelid".

uint8_t *upperOpen = NULL, *upperClosed = NULL;
uint8_t *lowerOpen = NULL, *lowerClosed = NULL;

const uint16_t *irisData = NULL, *scleraData = NULL;
static uint16_t s_irisW = 0, s_irisH = 0, s_scleraW = 0, s_scleraH = 0;
static uint16_t s_irisSolid = 0, s_scleraSolid = 0;   // 1x1 fallback storage

uint16_t irisWidth(void)    { return s_irisW; }
uint16_t irisHeight(void)   { return s_irisH; }
uint16_t scleraWidth(void)  { return s_scleraW; }
uint16_t scleraHeight(void) { return s_scleraH; }

// Adapter so the BMP loaders can read an SdFat File32. Note seekSet() rather
// than seek(), and read() returns a signed count (-1 on error).
class FileBmpReader : public BmpReader {
  File32 f;
public:
  explicit FileBmpReader(const char *path) {
    if (fsMounted) f = fatfs.open(path, FILE_READ);
  }
  ~FileBmpReader() { if (f) f.close(); }
  bool ok() const { return (bool)f; }
  bool seek(uint32_t pos) override { return f && f.seekSet(pos); }
  size_t read(void *buf, size_t len) override {
    if (!f) return 0;
    int n = f.read(buf, len);
    return (n < 0) ? 0 : (size_t)n;
  }
};

static void loadOneEyelid(const char *path, uint8_t *openT, uint8_t *closedT,
                          int size, bool isUpper) {
  const char *label = isUpper ? "upper" : "lower";
  if (path && path[0]) {
    FileBmpReader r(path);
    if (r.ok() && bmpLoadEyelid(r, openT, closedT, size, isUpper)) {
      Serial.printf("  %s eyelid: %s\n", label, path);
      return;
    }
    Serial.printf("  %s eyelid: %s unusable -- no eyelid\n", label, path);
  } else {
    Serial.printf("  %s eyelid: none specified\n", label);
  }
  // Init values mean "lid fully out of the way"
  memset(openT,   isUpper ? (uint8_t)(size - 1) : 0, size);
  memset(closedT, isUpper ? (uint8_t)(size - 1) : 0, size);
}

static bool loadOneTexture(const char *path, const uint16_t **data,
                           uint16_t *w, uint16_t *h, uint32_t budget,
                           uint16_t *solidStore, uint16_t solidColor,
                           const char *label) {
  if (path && path[0] && budget > 512) {
    FileBmpReader r(path);
    uint16_t *loaded = NULL;
    if (r.ok() && bmpLoadTexture(r, &loaded, w, h, budget)) {
      *data = loaded;
      Serial.printf("  %s: %s -> %ux%u (%u bytes)\n", label, path, *w, *h,
                    (unsigned)(*w * *h * 2));
      return true;
    }
    Serial.printf("  %s: %s unusable -- solid color\n", label, path);
  } else if (path && path[0]) {
    Serial.printf("  %s: no RAM for %s -- solid color\n", label, path);
  } else {
    Serial.printf("  %s: none specified -- solid color\n", label);
  }
  *solidStore = solidColor;      // 1x1 texture, exactly as the original does
  *data = solidStore;
  *w = *h = 1;
  return false;
}

bool eyeMediaLoad(int size, uint32_t texBudget) {
  uint8_t *block = (uint8_t *)malloc((size_t)size * 4);   // All four tables
  if (!block) return false;
  upperOpen   = &block[0];
  upperClosed = &block[size];
  lowerOpen   = &block[size * 2];
  lowerClosed = &block[size * 3];

  Serial.println("Media:");
  loadOneEyelid(settings.upperFile, upperOpen, upperClosed, size, true);
  loadOneEyelid(settings.lowerFile, lowerOpen, lowerClosed, size, false);

  // The sclera is usually a thin gradient; give the iris nearly everything.
  uint32_t scleraBudget = texBudget / 8;
  if (scleraBudget > 4096) scleraBudget = 4096;

  loadOneTexture(settings.irisFile, &irisData, &s_irisW, &s_irisH,
                 texBudget - scleraBudget, &s_irisSolid, settings.irisColor,
                 "iris");
  loadOneTexture(settings.scleraFile, &scleraData, &s_scleraW, &s_scleraH,
                 scleraBudget, &s_scleraSolid, settings.scleraColor, "sclera");
  return true;
}
