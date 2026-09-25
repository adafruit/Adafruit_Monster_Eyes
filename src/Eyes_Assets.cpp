/**
 * @file Eyes_Assets.cpp
 * @brief Everything that touches the CIRCUITPY drive.
 *
 * Sections, in order:
 *   1. BMP loading  -- streaming, 1-bit eyelids and 24-bit textures
 *   2. Storage      -- FatFS mount and USB drive mode
 *   3. Config       -- config.eye JSON
 *   4. Media        -- ties files to the renderer, with solid-colour fallback
 *
 * This is the ONLY translation unit that sees Adafruit_SPIFlash, SdFat,
 * TinyUSB or ArduinoJson, which keeps those four libraries' headers away from
 * the file holding the hot render loop. The flash, volume and MSC objects are
 * file-static rather than class members for the same reason: the public header
 * would otherwise need SdFat's types.
 *
 * Every asset is optional. A missing texture becomes a 1x1 solid colour, which
 * the renderer samples correctly and which still yields a properly sized,
 * dilating pupil; a missing eyelid leaves the sweep tables at their init
 * values, which reads as no eyelid.
 *
 * @note Requires ArduinoJson 7.x (JsonDocument). On 6.x the declaration in
 *       loadConfig() becomes StaticJsonDocument<2048>.
 */

#define ARDUINOJSON_ENABLE_COMMENTS 1 ///< Allow // comments inside config.eye

#include "Adafruit_Monster_Eyes.h"
#include "SdFat_Adafruit_Fork.h"
#include <Adafruit_SPIFlash.h>
#include <Adafruit_TinyUSB.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
//  WHICH FLASH REGION HOLDS THE FILESYSTEM
// ---------------------------------------------------------------------------
//
// Adapted from Adafruit's flash_config.h.

#if defined(ARDUINO_ARCH_RP2040) // Also RP2350 under arduino-pico
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
#ifndef MONSTER_EYES_CIRCUITPY_PARTITION
/** Use the CircuitPython flash layout rather than the core FS. */
#define MONSTER_EYES_CIRCUITPY_PARTITION 1
#endif
#if MONSTER_EYES_CIRCUITPY_PARTITION
static Adafruit_FlashTransport_RP2040_CPY flashTransport;
#else
static Adafruit_FlashTransport_RP2040 flashTransport;
#endif

#elif defined(ARDUINO_ARCH_ESP32)
// The ESP32 keeps its filesystem in a FAT partition of the same flash as the
// program. This transport locates it by parsing the partition table, so
// Tools > Partition Scheme MUST include a FATFS partition -- for example
// "Default 4MB with ffat". Without one, flash.begin() fails and the eye falls
// back to built-in defaults (a solid-colour eye), which is the symptom to look
// for.
static Adafruit_FlashTransport_ESP32 flashTransport;

#elif defined(EXTERNAL_FLASH_USE_QSPI)
static Adafruit_FlashTransport_QSPI flashTransport;

#elif defined(EXTERNAL_FLASH_USE_SPI)
static Adafruit_FlashTransport_SPI flashTransport(EXTERNAL_FLASH_USE_CS,
                                                  EXTERNAL_FLASH_USE_SPI);
#else
#error "No flash transport for this board -- add a branch to Eyes_Assets.cpp"
#endif

static Adafruit_SPIFlash flash(&flashTransport); ///< Flash chip driver
static FatVolume fatfs;        ///< FAT volume holding config.eye and bitmaps
static bool fsMounted = false; ///< Is the volume currently readable?

// ===========================================================================
//  1. BMP LOADING
// ===========================================================================

// Angular resolution past 512 is wasted: the renderer indexes as
// (angle * width / 1024) with angle 0-1023, and distance is 0-127.
#define TEX_MAX_W 512 ///< Widest texture the renderer can address
#define TEX_MAX_H 128 ///< Tallest texture the renderer can address

/** @brief Header fields the BMP loaders need from a bitmap. */
struct BmpInfo {
  int32_t width;       ///< Image width in pixels
  int32_t height;      ///< Image height, always positive; see #topDown
  uint16_t bpp;        ///< Bits per pixel; only 1 and 24 are supported
  uint32_t dataOffset; ///< Byte offset of the first pixel row
  uint32_t rowSize;    ///< Bytes per row, padded to a 4-byte boundary
  bool topDown;        ///< true if rows are stored first-to-last
  uint8_t whiteIndex;  ///< 1-bit only: the lighter of the two palette entries
};

static uint16_t rd16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint32_t rd32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static bool bmpReadHeader(BmpReader &r, BmpInfo &info) {
  uint8_t hdr[54];
  if (!r.seek(0))
    return false;
  if (r.read(hdr, sizeof(hdr)) != sizeof(hdr))
    return false;
  if ((hdr[0] != 'B') || (hdr[1] != 'M'))
    return false;

  info.dataOffset = rd32(&hdr[10]);
  const uint32_t dibSize = rd32(&hdr[14]);
  const int32_t w = (int32_t)rd32(&hdr[18]);
  const int32_t h = (int32_t)rd32(&hdr[22]);
  info.bpp = rd16(&hdr[28]);
  const uint32_t compression = rd32(&hdr[30]);

  if (dibSize < 40)
    return false; // BITMAPINFOHEADER+
  if (compression != 0)
    return false; // BI_RGB only
  if ((info.bpp != 1) && (info.bpp != 24))
    return false;
  if (w <= 0)
    return false;

  info.topDown = (h < 0); // Negative height means top-down rows
  info.height = info.topDown ? -h : h;
  info.width = w;
  if (info.height <= 0)
    return false;

  info.rowSize = (((uint32_t)w * info.bpp + 31) / 32) * 4; // 4-byte padded

  info.whiteIndex = 1;
  if (info.bpp == 1) {
    uint8_t pal[8]; // 2 entries, each B,G,R,reserved
    if (!r.seek(14 + dibSize))
      return false;
    if (r.read(pal, sizeof(pal)) != sizeof(pal))
      return false;
    const int lum0 = pal[0] + pal[1] + pal[2];
    const int lum1 = pal[4] + pal[5] + pal[6];
    info.whiteIndex = (lum1 > lum0) ? 1 : 0;
  }
  return true;
}

// Mirrors loadEyelid() in M4_Eyes' file.cpp: per column, find the topmost and
// bottommost lit pixel, then flip into render space where +Y is up. Unlike the
// original, which centred and CLIPPED against a fixed 240px screen, this
// scales proportionally so any source size fits any eye size.
static bool bmpLoadEyelid(BmpReader &r, uint8_t *openTable,
                          uint8_t *closedTable, int size, bool isUpper) {
  BmpInfo info;
  if (!bmpReadHeader(r, info))
    return false;
  if (info.bpp != 1)
    return false;
  if (info.height < 2)
    return false;

  const uint8_t init = isUpper ? (uint8_t)(size - 1) : 0;
  memset(openTable, init, size);
  memset(closedTable, init, size);

  uint16_t *minRow = (uint16_t *)malloc((size_t)size * 2 * sizeof(uint16_t));
  if (!minRow)
    return false;
  uint16_t *maxRow = &minRow[size];
  for (int i = 0; i < size; i++) {
    minRow[i] = 0xFFFF;
    maxRow[i] = 0;
  }

  uint8_t *row = (uint8_t *)malloc(info.rowSize);
  if (!row) {
    free(minRow);
    return false;
  }

  bool ok = true;
  for (int32_t fileRow = 0; fileRow < info.height; fileRow++) {
    if (!r.seek(info.dataOffset + (uint32_t)fileRow * info.rowSize) ||
        (r.read(row, info.rowSize) != info.rowSize)) {
      ok = false;
      break;
    }

    // Bottom-up is the BMP default: file row 0 is the image's last row.
    const int32_t imageRow =
        info.topDown ? fileRow : (info.height - 1 - fileRow);

    for (int32_t sx = 0; sx < info.width; sx++) {
      const uint8_t bit = (row[sx >> 3] >> (7 - (sx & 7))) & 1;
      if (bit != info.whiteIndex)
        continue;
      int dx = (int)((int64_t)sx * size / info.width);
      if (dx < 0)
        dx = 0;
      else if (dx >= size)
        dx = size - 1;
      if ((uint16_t)imageRow < minRow[dx])
        minRow[dx] = (uint16_t)imageRow;
      if ((uint16_t)imageRow > maxRow[dx])
        maxRow[dx] = (uint16_t)imageRow;
    }
  }
  free(row);

  if (ok) {
    for (int dx = 0; dx < size; dx++) {
      if (minRow[dx] == 0xFFFF)
        continue; // No data; keep the init value
      int my = (int)((int64_t)minRow[dx] * (size - 1) / (info.height - 1));
      int My = (int)((int64_t)maxRow[dx] * (size - 1) / (info.height - 1));
      if (my < 0)
        my = 0;
      else if (my > size - 1)
        my = size - 1;
      if (My < 0)
        My = 0;
      else if (My > size - 1)
        My = size - 1;
      if (isUpper) {
        openTable[dx] = (uint8_t)(size - 1 - my);
        closedTable[dx] = (uint8_t)(size - 1 - My);
      } else {
        closedTable[dx] = (uint8_t)(size - 1 - my);
        openTable[dx] = (uint8_t)(size - 1 - My);
      }
    }
  }
  free(minRow);
  return ok;
}

// Oversized images lose resolution rather than being rejected, so any source
// works on any board.
static bool bmpLoadTexture(BmpReader &r, uint16_t **data, uint16_t *width,
                           uint16_t *height, uint32_t maxBytes) {
  BmpInfo info;
  if (!bmpReadHeader(r, info))
    return false;
  if (info.bpp != 24)
    return false;

  // Start at the source size capped to what the renderer can address, then
  // shrink the longer dimension until it fits the budget.
  int dw = (int)info.width < TEX_MAX_W ? (int)info.width : TEX_MAX_W;
  int dh = (int)info.height < TEX_MAX_H ? (int)info.height : TEX_MAX_H;
  while (((uint32_t)dw * dh * 2 > maxBytes) && ((dw > 8) || (dh > 4))) {
    if ((dw * (int)info.height) > (dh * (int)info.width)) {
      if (dw > 8)
        dw--;
      else
        dh--;
    } else {
      if (dh > 4)
        dh--;
      else
        dw--;
    }
  }
  if ((uint32_t)dw * dh * 2 > maxBytes)
    return false;

  uint16_t *dst = (uint16_t *)eyesMalloc((size_t)dw * dh * 2);
  if (!dst)
    return false;
  uint8_t *row = (uint8_t *)malloc(info.rowSize);
  if (!row) {
    free(dst);
    return false;
  }

  bool ok = true;
  for (int dy = 0; dy < dh; dy++) {
    const int32_t imageRow = (int32_t)((int64_t)dy * info.height / dh);
    const int32_t fileRow =
        info.topDown ? imageRow : (info.height - 1 - imageRow);
    if (!r.seek(info.dataOffset + (uint32_t)fileRow * info.rowSize) ||
        (r.read(row, info.rowSize) != info.rowSize)) {
      ok = false;
      break;
    }

    uint16_t *out = &dst[(size_t)dy * dw];
    for (int dx = 0; dx < dw; dx++) {
      const int32_t sx = (int32_t)((int64_t)dx * info.width / dw);
      const uint8_t *p = &row[(size_t)sx * 3]; // Stored B, G, R
      *out++ =
          (uint16_t)(((p[2] & 0xF8) << 8) | ((p[1] & 0xFC) << 3) | (p[0] >> 3));
    }
  }
  free(row);
  if (!ok) {
    free(dst);
    return false;
  }

  *data = dst;
  *width = (uint16_t)dw;
  *height = (uint16_t)dh;
  return true;
}

// Adapter so the BMP loaders can read an SdFat File32. Note seekSet() rather
// than seek(), and read() returns a signed count (-1 on error).
/** @brief Adapts an SdFat File32 to the BmpReader interface. */
class FileBmpReader : public BmpReader {
  File32 f; ///< Open file, or a closed handle if the path did not exist

public:
  /**
   * @brief Open a file on the mounted volume.
   * @param path Absolute path on the asset filesystem.
   */
  explicit FileBmpReader(const char *path) {
    if (fsMounted)
      f = fatfs.open(path, FILE_READ);
  }
  ~FileBmpReader() {
    if (f)
      f.close();
  }
  /** @brief Did the file open? @return true if open and readable. */
  bool ok() const { return (bool)f; }
  bool seek(uint32_t pos) override { return f && f.seekSet(pos); }
  size_t read(void *buf, size_t len) override {
    if (!f)
      return 0;
    const int n = f.read(buf, len);
    return (n < 0) ? 0 : (size_t)n;
  }
};

// ===========================================================================
//  2. STORAGE
// ===========================================================================

static Adafruit_USBD_MSC usb_msc;
static volatile bool mscWritten = false;
static volatile uint32_t lastWriteMillis = 0;

// These three run in USB interrupt context. Keep them to block I/O only.
static int32_t mscReadCb(uint32_t lba, void *buffer, uint32_t bufsize) {
  return flash.readBlocks(lba, (uint8_t *)buffer, bufsize / 512)
             ? (int32_t)bufsize
             : -1;
}

static int32_t mscWriteCb(uint32_t lba, uint8_t *buffer, uint32_t bufsize) {
  mscWritten = true;
  lastWriteMillis = millis();
  return flash.writeBlocks(lba, buffer, bufsize / 512) ? (int32_t)bufsize : -1;
}

static void mscFlushCb(void) {
  flash.syncBlocks();
  fatfs.cacheClear();
  lastWriteMillis = millis();
}

bool Adafruit_Monster_Eyes::storageBegin(void) {
  if (!flash.begin()) {
    EYES_ERR("Flash chip init failed.\n");
    fsMounted = false;
    return false;
  }
  EYES_DBG("Flash JEDEC ID 0x%06lX, %lu bytes\n",
           (unsigned long)flash.getJEDECID(), (unsigned long)flash.size());

  if (!fatfs.begin(&flash)) {
    EYES_ERR("No FAT filesystem found on the flash partition.\n");
    EYES_ERR("Either load CircuitPython once to create CIRCUITPY, or hold the "
             "button at reset and let the host format the drive.\n");
    fsMounted = false;
    return false;
  }
  fsMounted = true;
  return true;
}

void Adafruit_Monster_Eyes::storageEnd(void) { fsMounted = false; }

bool Adafruit_Monster_Eyes::storageMounted(void) const { return fsMounted; }

bool Adafruit_Monster_Eyes::driveModeRequested(void) {
  return eyesSafeModeRequested(_safeModePin);
}

void Adafruit_Monster_Eyes::runDriveMode(void) {
  EYES_DBG("=== USB DRIVE MODE ===\n");
  EYES_DBG("The display is intentionally off. Copy files, then eject.\n");

  if (!flash.begin()) {
    EYES_ERR("Flash chip init failed; cannot export a drive.\n");
    for (;;)
      delay(1000);
  }
  fsMounted = fatfs.begin(&flash);
  if (!fsMounted)
    EYES_ERR("Volume not mountable -- format it from the host.\n");

  usb_msc.setID("Adafruit", "Eye Assets", "1.0");
  usb_msc.setCapacity(flash.size() / 512, 512);
  usb_msc.setReadWriteCallback(mscReadCb, mscWriteCb, mscFlushCb);
  usb_msc.setUnitReady(true);
  usb_msc.begin();

  EYES_DBG("Drive exported. Rebooting automatically once writes stop.\n");

#ifdef LED_BUILTIN
  pinMode(LED_BUILTIN, OUTPUT);
#endif
  uint32_t lastBlink = 0;
  bool ledState = false;

  for (;;) {
    if (mscWritten && ((millis() - lastWriteMillis) > 2000)) {
      EYES_DBG("Writes finished -- rebooting into eye mode.\n");
      flash.syncBlocks();
      delay(250);
      eyesReboot();
    }
    const uint32_t now = millis();
    const uint32_t period = mscWritten ? 120 : 600; // Fast blink after a write
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
//  3. CONFIG
// ===========================================================================

// "Do What I Mean" decoder from M4_Eyes' file.cpp. Accepts 42, "0x2A",
// "0xF800", [255,0,0], ["0xFF",0,0], [1.0,0.0,0.0]. Unlike the original this
// returns NATIVE-endian RGB565; the backend converts on the way out.
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
        if (a[i].is<int>())
          cc[i] = a[i].as<int>();
        else if (a[i].is<float>())
          cc[i] = (long)(a[i].as<float>() * 255.999f);
        else if (a[i].is<const char *>())
          cc[i] = strtol(a[i].as<const char *>(), NULL, 0);
        else
          cc[i] = 0;
        if (cc[i] > 255)
          cc[i] = 255;
        else if (cc[i] < 0)
          cc[i] = 0;
      }
      return ((cc[0] & 0xF8) << 8) | ((cc[1] & 0xFC) << 3) | (cc[2] >> 3);
    }
    if (a.size() >= 1) {
      if (a[0].is<int>())
        return a[0].as<int>();
      return strtol(a[0].as<const char *>(), NULL, 0);
    }
  }
  return def;
}

static void copyStr(char *dst, JsonVariantConst v) {
  if (v.is<const char *>()) {
    strncpy(dst, v.as<const char *>(), EYES_PATH_MAX - 1);
    dst[EYES_PATH_MAX - 1] = 0;
  }
}

// Apply one JSON object: the document root, or a per-eye sub-object on top.
void Adafruit_Monster_Eyes::applyConfigRoot(const void *variantPtr) {
  JsonVariantConst o = *(const JsonVariantConst *)variantPtr;
  if (o.isNull())
    return;
  JsonVariantConst v;

  _settings.displaySize = dwim(o["displaySize"], _settings.displaySize);
  _settings.eyeRadius = dwim(o["eyeRadius"], _settings.eyeRadius);
  _settings.irisRadius = dwim(o["irisRadius"], _settings.irisRadius);
  _settings.slitPupilRadius =
      dwim(o["slitPupilRadius"], _settings.slitPupilRadius);
  _settings.gazeMax = (uint32_t)dwim(o["gazeMax"], (int32_t)_settings.gazeMax);
  _settings.fixate = dwim(o["fixate"], _settings.fixate);

  v = o["coverage"];
  if (v.is<float>() || v.is<int>()) {
    _settings.coverage = v.as<float>();
    _settings.coverageRequested = _settings.coverage;
  }

  _settings.pupilColor = (uint16_t)dwim(o["pupilColor"], _settings.pupilColor);
  _settings.backColor = (uint16_t)dwim(o["backColor"], _settings.backColor);
  _settings.irisColor = (uint16_t)dwim(o["irisColor"], _settings.irisColor);
  _settings.scleraColor =
      (uint16_t)dwim(o["scleraColor"], _settings.scleraColor);

  // Legacy eyelidIndex expands to a grey via index * 0x0101, which is
  // byte-symmetric and so survives the endianness change untouched. A full
  // 16-bit eyelidColor is also accepted.
  v = o["eyelidIndex"];
  if (!v.isNull())
    _settings.eyelidColor = (uint16_t)(dwim(v) & 0xFF) * 0x0101;
  v = o["eyelidColor"];
  if (!v.isNull())
    _settings.eyelidColor = (uint16_t)dwim(v, _settings.eyelidColor);

  v = o["pupilMin"];
  if (v.is<float>() || v.is<int>())
    _settings.pupilMin = v.as<float>();
  v = o["pupilMax"];
  if (v.is<float>() || v.is<int>())
    _settings.pupilMax = v.as<float>();
  v = o["tracking"];
  if (v.is<bool>())
    _settings.tracking = v.as<bool>();
  v = o["squint"];
  if (v.is<float>() || v.is<int>())
    _settings.trackFactor = 1.0f - v.as<float>();

  v = o["irisSpin"];
  if (v.is<float>() || v.is<int>())
    _settings.irisSpin = v.as<float>();
  v = o["scleraSpin"];
  if (v.is<float>() || v.is<int>())
    _settings.scleraSpin = v.as<float>();

  v = o["irisAngle"];
  if (v.is<int>())
    _settings.irisStartAngle = 1023 - (v.as<int>() & 1023);
  else if (v.is<float>())
    _settings.irisStartAngle = 1023 - ((int)(v.as<float>() * 1024.0f) & 1023);
  v = o["scleraAngle"];
  if (v.is<int>())
    _settings.scleraStartAngle = 1023 - (v.as<int>() & 1023);
  else if (v.is<float>())
    _settings.scleraStartAngle = 1023 - ((int)(v.as<float>() * 1024.0f) & 1023);

  v = o["irisMirror"];
  if (v.is<bool>() || v.is<int>())
    _settings.irisMirror = v.as<bool>() ? 1023 : 0;
  v = o["scleraMirror"];
  if (v.is<bool>() || v.is<int>())
    _settings.scleraMirror = v.as<bool>() ? 1023 : 0;
  v = o["eyelidMirror"];
  if (v.is<bool>() || v.is<int>())
    _settings.eyelidMirror = v.as<bool>();

  copyStr(_settings.irisFile, o["irisTexture"]);
  copyStr(_settings.scleraFile, o["scleraTexture"]);
  copyStr(_settings.upperFile, o["upperEyelid"]);
  copyStr(_settings.lowerFile, o["lowerEyelid"]);
}

// Only the values that may legitimately differ between two eyes. Geometry and
// texture keys inside a "left"/"right" block are ignored in a two-eye build,
// because both eyes share one set of polar maps and one copy of each texture.
void Adafruit_Monster_Eyes::applyConfigVariant(const void *variantPtr,
                                               EyesVariant &v) {
  JsonVariantConst o = *(const JsonVariantConst *)variantPtr;
  if (o.isNull())
    return;
  JsonVariantConst x;
  x = o["irisSpin"];
  if (x.is<float>() || x.is<int>())
    v.irisSpin = x.as<float>();
  x = o["scleraSpin"];
  if (x.is<float>() || x.is<int>())
    v.scleraSpin = x.as<float>();
  x = o["irisAngle"];
  if (x.is<int>())
    v.irisStartAngle = 1023 - (x.as<int>() & 1023);
  else if (x.is<float>())
    v.irisStartAngle = 1023 - ((int)(x.as<float>() * 1024.0f) & 1023);
  x = o["scleraAngle"];
  if (x.is<int>())
    v.scleraStartAngle = 1023 - (x.as<int>() & 1023);
  else if (x.is<float>())
    v.scleraStartAngle = 1023 - ((int)(x.as<float>() * 1024.0f) & 1023);
  x = o["irisMirror"];
  if (x.is<bool>() || x.is<int>())
    v.irisMirror = x.as<bool>() ? 1023 : 0;
  x = o["scleraMirror"];
  if (x.is<bool>() || x.is<int>())
    v.scleraMirror = x.as<bool>() ? 1023 : 0;
  x = o["eyelidMirror"];
  if (x.is<bool>() || x.is<int>())
    v.eyelidMirror = x.as<bool>();
}

bool Adafruit_Monster_Eyes::loadConfig(const char *path) {
  if (!path)
    path = _configFile;
  if (!fsMounted)
    return false;
  File32 f = fatfs.open(path, FILE_READ);
  if (!f) {
    EYES_DBG("No %s on drive; using built-in defaults\n", path);
    return false;
  }
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) {
    EYES_ERR("Config parse error (%s); using built-in defaults\n", err.c_str());
    return false;
  }

  JsonVariantConst root = doc.as<JsonVariantConst>();
  applyConfigRoot(&root);
  if (_numEyes == 1) {
    // A single eye may take anything from its side's block, including geometry.
    JsonVariantConst side = doc[_sideRight ? "right" : "left"];
    applyConfigRoot(&side);
  }
  seedVariants();
  if (_numEyes > 1) {
    JsonVariantConst rightBlock = doc["right"];
    JsonVariantConst leftBlock = doc["left"];
    applyConfigVariant(&rightBlock, _variant[0]);
    applyConfigVariant(&leftBlock, _variant[1]);
  }
  EYES_DBG("Loaded %s\n", path);
  return true;
}

// ===========================================================================
//  4. MEDIA
// ===========================================================================

static void loadOneEyelid(const char *path, uint8_t *openT, uint8_t *closedT,
                          int size, bool isUpper) {
  const char *label = isUpper ? "upper" : "lower";
  (void)label;
  if (path && path[0]) {
    FileBmpReader r(path);
    if (r.ok() && bmpLoadEyelid(r, openT, closedT, size, isUpper)) {
      EYES_DBG("  %s eyelid: %s\n", label, path);
      return;
    }
    EYES_DBG("  %s eyelid: %s unusable -- no eyelid\n", label, path);
  } else {
    EYES_DBG("  %s eyelid: none specified\n", label);
  }
  // Init values mean "lid fully out of the way".
  memset(openT, isUpper ? (uint8_t)(size - 1) : 0, size);
  memset(closedT, isUpper ? (uint8_t)(size - 1) : 0, size);
}

// Release everything mediaLoad() allocated, and point the renderer back at
// the 1x1 solid-colour fallbacks so it stays valid in between.
//
// Only textures that came from a FILE are freed: when no file loaded, the
// pointer is the address of a member, not something malloc gave us.
void Adafruit_Monster_Eyes::freeMedia(void) {
  if (_lidBlock) {
    free(_lidBlock);
    _lidBlock = NULL;
  }
  _upperOpen = _upperClosed = _lowerOpen = _lowerClosed = NULL;

  if (_irisFromFile && _irisData)
    free((void *)_irisData);
  _irisFromFile = false;
  _irisSolid = out16(_settings.irisColor);
  _irisData = &_irisSolid;
  _irisW = _irisH = 1;

  if (_scleraFromFile && _scleraData)
    free((void *)_scleraData);
  _scleraFromFile = false;
  _scleraSolid = out16(_settings.scleraColor);
  _scleraData = &_scleraSolid;
  _scleraW = _scleraH = 1;
}

bool Adafruit_Monster_Eyes::mediaLoad(int size, uint32_t texBudget) {
  // Release the previous set first. Without this a second call leaks the old
  // textures, which on a 240px eye is over 100 KB a time.
  freeMedia();

  _lidBlock = (uint8_t *)eyesMalloc((size_t)size * 4); // All four tables
  if (!_lidBlock)
    return false;
  _upperOpen = &_lidBlock[0];
  _upperClosed = &_lidBlock[size];
  _lowerOpen = &_lidBlock[size * 2];
  _lowerClosed = &_lidBlock[size * 3];

  EYES_DBG("Media:\n");
  loadOneEyelid(_settings.upperFile, _upperOpen, _upperClosed, size, true);
  loadOneEyelid(_settings.lowerFile, _lowerOpen, _lowerClosed, size, false);

  uint32_t scleraBudget = texBudget / 8;
  if (scleraBudget > 4096)
    scleraBudget = 4096;

  // The iris gets the lion's share; the sclera is mostly flat colour anyway.
  struct {
    const char *path;
    const uint16_t **data;
    uint16_t *w;
    uint16_t *h;
    uint16_t *solid;
    uint16_t color;
    bool *fromFile;
    uint32_t budget;
    const char *label;
  } jobs[2] = {
      {_settings.irisFile, &_irisData, &_irisW, &_irisH, &_irisSolid,
       _settings.irisColor, &_irisFromFile,
       (texBudget > scleraBudget) ? (texBudget - scleraBudget) : 0, "iris"},
      {_settings.scleraFile, &_scleraData, &_scleraW, &_scleraH, &_scleraSolid,
       _settings.scleraColor, &_scleraFromFile, scleraBudget, "sclera"},
  };

  for (int i = 0; i < 2; i++) {
    *jobs[i].fromFile = false;
    if (jobs[i].path && jobs[i].path[0] && (jobs[i].budget > 512)) {
      FileBmpReader r(jobs[i].path);
      uint16_t *loaded = NULL;
      if (r.ok() &&
          bmpLoadTexture(r, &loaded, jobs[i].w, jobs[i].h, jobs[i].budget)) {
        if (_swapBytes) {
          // Swap once here so the render loop never has to, and so the bytes
          // are already wire-ready for a DMA.
          const uint32_t n = (uint32_t)(*jobs[i].w) * (*jobs[i].h);
          for (uint32_t p = 0; p < n; p++)
            loaded[p] = __builtin_bswap16(loaded[p]);
        }
        *jobs[i].data = loaded;
        *jobs[i].fromFile = true;
        EYES_DBG("  %s: %s -> %ux%u (%u bytes)\n", jobs[i].label, jobs[i].path,
                 *jobs[i].w, *jobs[i].h,
                 (unsigned)(*jobs[i].w * *jobs[i].h * 2));
        continue;
      }
      EYES_DBG("  %s: %s unusable -- solid colour\n", jobs[i].label,
               jobs[i].path);
    } else if (jobs[i].path && jobs[i].path[0]) {
      EYES_DBG("  %s: no RAM for %s -- solid colour\n", jobs[i].label,
               jobs[i].path);
    } else {
      EYES_DBG("  %s: none specified -- solid colour\n", jobs[i].label);
    }
    // 1x1 texture, exactly as M4_Eyes does -- but in the byte order the
    // backend wants, since the renderer samples it without conversion.
    *jobs[i].solid = out16(jobs[i].color);
    *jobs[i].data = jobs[i].solid;
    *jobs[i].w = *jobs[i].h = 1;
  }
  return true;
}
