/**
 * @file Adafruit_Monster_Eyes.cpp
 * @brief Core: construction, startup sequencing, animation, the renderer and
 *        the polar/displacement tables.
 *
 * The tables live here rather than with the asset loading because they need
 * nothing but arithmetic and the settings -- no filesystem, no JSON -- which
 * keeps the translation unit holding the hot render loop free of the four
 * large libraries the asset path pulls in.
 *
 * Ported from Adafruit's M4_Eyes by Phillip Burgess.
 */

#include "Adafruit_Monster_Eyes.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

Stream *eyesLogStream = NULL;

#define NOBLINK 0 ///< Lids at rest
#define ENBLINK 1 ///< Lids closing
#define DEBLINK 2 ///< Lids opening

#define IRIS_LEVELS 7 ///< Subdivision levels in the fractal iris animator

// ===========================================================================
//  CONSTRUCTION
// ===========================================================================

Adafruit_Monster_Eyes::Adafruit_Monster_Eyes(Adafruit_SPITFT *panel0,
                                             Adafruit_SPITFT *panel1) {
  _display = new Eyes_TFT(panel0, panel1);
  _ownsDisplay = true;
  _isTft = true;
  _numEyes = _display->eyeCount();
  applyDefaults();
}

#if defined(ARDUINO_ARCH_RP2040)
Adafruit_Monster_Eyes::Adafruit_Monster_Eyes(DVIGFX16 *dvi, uint8_t numEyes) {
  _display = new Eyes_DVI(dvi, numEyes);
  _ownsDisplay = true;
  _isTft = false;
  _numEyes = _display->eyeCount();
  applyDefaults();
}
#endif

#if defined(ARDUINO_ARCH_ESP32) &&                                             \
    (CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32P4)
Adafruit_Monster_Eyes::Adafruit_Monster_Eyes(Arduino_RGB_Display *gfx,
                                             uint8_t scale) {
  _display = new Eyes_RGB666(gfx, scale);
  _ownsDisplay = true;
  _isTft = false;
  _numEyes = _display->eyeCount();
  applyDefaults();
}
#endif

Adafruit_Monster_Eyes::Adafruit_Monster_Eyes(Eyes_Display *display) {
  _display = display;
  _ownsDisplay = false;
  _isTft = false;
  _numEyes = display ? display->eyeCount() : 1;
  applyDefaults();
}

Adafruit_Monster_Eyes::~Adafruit_Monster_Eyes() {
  tablesFree();
  freeMedia();
  if (_ownsDisplay && _display)
    delete _display;
}

// Built-in defaults. These live in the CONSTRUCTOR rather than at the top of
// begin(), because setters called before begin() have to survive it -- they
// are the sketch's stated preferences, and only config.eye outranks them.
void Adafruit_Monster_Eyes::applyDefaults(void) {
  memset(&_settings, 0, sizeof(_settings));

  _settings.displaySize = 0;     // Fill whatever the display can give one eye
  _settings.eyeRadius = 0;       // Derive from displaySize
  _settings.irisRadius = 0;      // Derive from displaySize
  _settings.slitPupilRadius = 0; // Round pupil
  _settings.coverage = 0.6f;
  _settings.coverageRequested = _settings.coverage;

  _settings.pupilColor = 0x0000;
  _settings.backColor = 0x5000;
  _settings.eyelidColor = 0x0000;
  _settings.irisColor = 0x001F;
  _settings.scleraColor = 0xFFFF;

  _settings.pupilMin = 0.05f;
  _settings.pupilMax = 0.25f;
  _settings.tracking = true;
  _settings.trackFactor = 0.5f;
  _settings.gazeMax = 3000000;
  _settings.irisSpin = 0.0f;
  _settings.scleraSpin = 0.0f;
  _settings.irisStartAngle = 512;
  _settings.scleraStartAngle = 512;
  _settings.eyelidMirror = true;
  _settings.fixate = 7;

  _swapBytes = false;
  _begun = false;
  _error = NULL;

  _size = _half = 0;
  _gazeRadius = 1.0f;

  _displace = NULL;
  _polarAngle = NULL;
  _polarDist = NULL;
  _mapRadius = _mapDiameter = 0;

  _lidBlock = NULL;
  _upperOpen = _upperClosed = _lowerOpen = _lowerClosed = NULL;
  _irisData = _scleraData = NULL;
  _irisW = _irisH = _scleraW = _scleraH = 1;
  _irisSolid = _scleraSolid = 0;
  _irisFromFile = _scleraFromFile = false;

  _eyeInMotion = false;
  _eyeOldX = _eyeOldY = _eyeNewX = _eyeNewY = 0.0f;
  _eyeMoveStartTime = 0;
  _eyeMoveDuration = 0;
  _lastSaccadeStop = 0;
  _saccadeInterval = 0;
  _timeOfLastBlink = _timeToNextBlink = 0;
  _frameEyeX = _frameEyeY = 0.0f;

  for (int i = 0; i < IRIS_LEVELS; i++)
    _irisPrev[i] = _irisNext[i] = 0.0f;
  _irisFrame = 0;
  _irisValue = 0.5f;
  _irisMin = 0.0f;
  _irisRange = 1.0f;

  _gazeExternal = _pupilExternal = _blinkExternal = false;
  _autoBlink = _autoGaze = true;
  _blinkForced = 0.0f;
  _clockOffset = 0;

  _configFile = "/config.eye";
  _storageEnabled = true;
  _storageKeep = false;
  _driveModeEnabled = true;
  _safeModePin = EYES_SAFE_MODE_PIN_DEFAULT;
  _sideRight = false;
  _sideSet = false;
  _selfTest = false;
  _profile = false;

  _frames = 0;
  _lastRateReport = 0;
  _lastRenderMs = _lastTransferMs = 0.0f;
  _frameMicros = 0;

  seedVariants();
}

void Adafruit_Monster_Eyes::setVerbose(Stream &stream) {
  eyesLogStream = &stream;
}

void Adafruit_Monster_Eyes::setProfile(bool on) {
  _profile = on;
  if (_display)
    _display->setProfile(on);
}

void Adafruit_Monster_Eyes::setFastSPI(int sck0, int sck1, bool useDma) {
  if (_isTft && _display)
    static_cast<Eyes_TFT *>(_display)->setFastSPI(sck0, sck1, useDma);
}

void Adafruit_Monster_Eyes::resetPanels(int rst0, int rst1) {
  const int pins[2] = {rst0, rst1};
  Eyes_Display::resetPanels(pins, (rst1 >= 0) ? 2 : 1);
}

void Adafruit_Monster_Eyes::fail(const char *why) {
  _error = why;
  EYES_ERR("Monster Eyes: %s\n", why);
}

// ===========================================================================
//  SETTINGS
// ===========================================================================

// Seed each eye's variant from the shared settings. With two eyes, eye 0 (the
// character's right, appearing on the viewer's left) is the mirror of eye 1:
// opposite iris rotation, opposite start angle, unmirrored eyelids.
static void applyRightEyeOrientation(EyesVariant &v) {
  v.irisSpin = -v.irisSpin;
  v.scleraSpin = -v.scleraSpin;
  v.irisStartAngle = (uint16_t)((v.irisStartAngle + 512) & 1023);
  v.eyelidMirror = !v.eyelidMirror;
}

void Adafruit_Monster_Eyes::seedVariants(void) {
  for (int e = 0; e < _numEyes; e++) {
    EyesVariant &v = _variant[e];
    v.irisSpin = _settings.irisSpin;
    v.scleraSpin = _settings.scleraSpin;
    v.irisStartAngle = _settings.irisStartAngle;
    v.scleraStartAngle = _settings.scleraStartAngle;
    v.irisMirror = _settings.irisMirror;
    v.scleraMirror = _settings.scleraMirror;
    v.eyelidMirror = _settings.eyelidMirror;
    if (_numEyes > 1) {
      if (e == 0)
        applyRightEyeOrientation(v);
    } else if (_sideRight) {
      applyRightEyeOrientation(v);
    }
  }
}

void Adafruit_Monster_Eyes::finalizeSettings(bool announce) {
  // 0 means "fill whatever the display can give one eye"; begin() resolves it
  // once the backend is up. Anything else is clamped to a sane range.
  if (_settings.displaySize != 0) {
    if (_settings.displaySize < 64)
      _settings.displaySize = 64;
    if (_settings.displaySize > 240)
      _settings.displaySize = 240;
    _settings.displaySize &= ~1; // Keep even; the renderer halves it
  }

  if (_settings.eyeRadius <= 0)
    _settings.eyeRadius = _settings.displaySize / 2 + 5;
  else
    _settings.eyeRadius = abs(_settings.eyeRadius);

  // Auto values keep the stock demon proportions at ANY displaySize, so a
  // config can change size alone and stay geometrically consistent:
  //   eyeRadius       240 -> 125   (displaySize/2 + 5)
  //   irisRadius      240 -> 110   (0.4583 * displaySize)
  //   slitPupilRadius 240 -> 100   (0.4167 * displaySize)
  // A mismatch between these is what lets the iris wander out of frame, so
  // leaving them at 0 / -1 is the safest way to resize the eye.
  if (_settings.irisRadius <= 0)
    _settings.irisRadius = (int)(0.4583f * (float)_settings.displaySize + 0.5f);
  else
    _settings.irisRadius = abs(_settings.irisRadius);
  // screen2map() takes sqrt(eyeRadius^2 - irisRadius^2); keep it real.
  if (_settings.irisRadius >= _settings.eyeRadius)
    _settings.irisRadius = _settings.eyeRadius - 1;

  // 0 means a round pupil, so negative is the "auto" signal here.
  if (_settings.slitPupilRadius < 0)
    _settings.slitPupilRadius =
        (int)(0.4167f * (float)_settings.displaySize + 0.5f);
  if (_settings.slitPupilRadius > _settings.irisRadius)
    _settings.slitPupilRadius = _settings.irisRadius;

  // COVERAGE MUST BE LARGE ENOUGH FOR THE EYE TO LOOK AROUND.
  //
  // The gaze travels within a disc of radius
  //     (mapDiameter - displaySize * pi/2) * 0.75
  // and mapRadius is eyeRadius * pi * coverage. If eyeRadius is small relative
  // to displaySize -- which happens when a config sets a new displaySize but
  // leaves eyeRadius at a value scaled for the old one -- that radius goes to
  // zero and then negative, and the eye drifts until the iris slides off the
  // eyeball. Solving for the ratio the stock 240px demon eye uses (a gaze
  // radius of about 0.30 * mapRadius) gives mapRadius ~= 0.98 * displaySize.
  _settings.coverage = _settings.coverageRequested;
  if (_settings.displaySize == 0)
    return; // Not resolved yet; nothing to check
  const float wantMapRadius = 0.9818f * (float)_settings.displaySize;
  const float needCoverage =
      wantMapRadius / ((float)_settings.eyeRadius * (float)M_PI);
  // Tolerance: the stock geometry lands within rounding distance of the
  // requirement, and warning about a 0.00003 shortfall is just noise.
  if (_settings.coverage < needCoverage * 0.98f) {
    if (announce) {
      EYES_DBG("coverage %.2f too low for eye size %d with eyeRadius %d; "
               "raising to %.2f\n",
               _settings.coverage, _settings.displaySize, _settings.eyeRadius,
               needCoverage);
      EYES_DBG("  (better fix: set eyeRadius near %d)\n",
               _settings.displaySize / 2 + 5);
    }
    _settings.coverage = needCoverage;
  }
  if (_settings.coverage < 0.05f)
    _settings.coverage = 0.05f;
  else if (_settings.coverage > 1.0f)
    _settings.coverage = 1.0f;

  if (_settings.pupilMin < 0.0f)
    _settings.pupilMin = 0.0f;
  if (_settings.pupilMax > 1.0f)
    _settings.pupilMax = 1.0f;
  if (_settings.pupilMin > _settings.pupilMax) {
    const float t = _settings.pupilMin;
    _settings.pupilMin = _settings.pupilMax;
    _settings.pupilMax = t;
  }
  if (_settings.trackFactor < 0.0f)
    _settings.trackFactor = 0.0f;
  else if (_settings.trackFactor > 1.0f)
    _settings.trackFactor = 1.0f;
}

static void copyPath(char *dst, const char *src) {
  if (!src) {
    dst[0] = 0;
    return;
  }
  strncpy(dst, src, EYES_PATH_MAX - 1);
  dst[EYES_PATH_MAX - 1] = 0;
}

void Adafruit_Monster_Eyes::setIrisTexture(const char *path) {
  copyPath(_settings.irisFile, path);
}
void Adafruit_Monster_Eyes::setScleraTexture(const char *path) {
  copyPath(_settings.scleraFile, path);
}
void Adafruit_Monster_Eyes::setUpperEyelid(const char *path) {
  copyPath(_settings.upperFile, path);
}
void Adafruit_Monster_Eyes::setLowerEyelid(const char *path) {
  copyPath(_settings.lowerFile, path);
}

void Adafruit_Monster_Eyes::setIrisColor(uint16_t c) {
  _settings.irisColor = c;
  // The solid colour is a 1x1 texture the renderer samples like any other, so
  // it has to be stored in the byte order the backend wants.
  if (!_irisFromFile) {
    _irisSolid = out16(c);
    _irisData = &_irisSolid;
    _irisW = _irisH = 1;
  }
}

void Adafruit_Monster_Eyes::setScleraColor(uint16_t c) {
  _settings.scleraColor = c;
  if (!_scleraFromFile) {
    _scleraSolid = out16(c);
    _scleraData = &_scleraSolid;
    _scleraW = _scleraH = 1;
  }
}

void Adafruit_Monster_Eyes::setPupilRange(float minFrac, float maxFrac) {
  _settings.pupilMin = minFrac;
  _settings.pupilMax = maxFrac;
  if (_begun) {
    _irisMin = 1.0f - _settings.pupilMax;
    _irisRange = _settings.pupilMax - _settings.pupilMin;
  }
}

void Adafruit_Monster_Eyes::setIrisSpin(float rpm, int eye) {
  if (eye < 0) {
    _settings.irisSpin = rpm;
    for (int e = 0; e < _numEyes; e++) {
      _variant[e].irisSpin = ((_numEyes > 1) && (e == 0)) ? -rpm : rpm;
      _eye[e].irisSpin = -1024.0f * _variant[e].irisSpin;
    }
  } else if (eye < _numEyes) {
    _variant[eye].irisSpin = rpm;
    _eye[eye].irisSpin = -1024.0f * rpm;
  }
}

void Adafruit_Monster_Eyes::setScleraSpin(float rpm, int eye) {
  if (eye < 0) {
    _settings.scleraSpin = rpm;
    for (int e = 0; e < _numEyes; e++) {
      _variant[e].scleraSpin = ((_numEyes > 1) && (e == 0)) ? -rpm : rpm;
      _eye[e].scleraSpin = -1024.0f * _variant[e].scleraSpin;
    }
  } else if (eye < _numEyes) {
    _variant[eye].scleraSpin = rpm;
    _eye[eye].scleraSpin = -1024.0f * rpm;
  }
}

// ===========================================================================
//  TABLES
// ===========================================================================
//
// Adapted from M4_Eyes' tablegen.cpp. The maths is unchanged; the sizes come
// from settings.
//
// The round eyeball is faked with a 2D displacement map rather than real 3D
// rotation. Both tables cover ONE QUADRANT and are mirrored at render time.

float Adafruit_Monster_Eyes::screen2map(int in) const {
  return atan2f((float)in,
                sqrtf((float)(_settings.eyeRadius * _settings.eyeRadius -
                              in * in))) /
         (float)M_PI_2 * (float)_mapRadius;
}

float Adafruit_Monster_Eyes::map2screen(int in) const {
  return sinf((float)in / (float)_mapRadius) * (float)M_PI_2 *
         (float)_settings.eyeRadius;
}

bool Adafruit_Monster_Eyes::calcDisplacement(void) {
  const int half = _settings.displaySize / 2;
  _displace = (uint8_t *)eyesMalloc((size_t)half * half);
  if (!_displace)
    return false;

  const float eyeRadius2 = (float)(_settings.eyeRadius * _settings.eyeRadius);
  uint8_t *ptr = _displace;

  // First quadrant only, "+Y is up". Pixel centres at +0.5 by design; that
  // makes mirroring numerically correct.
  for (int y = 0; y < half; y++) {
    float dy = (float)y + 0.5f;
    dy *= dy;
    for (int x = 0; x < half; x++) {
      float dx = (float)x + 0.5f;
      const float d2 = dx * dx + dy;
      if (d2 <= eyeRadius2) {
        const float d = sqrtf(d2);
        const float h = sqrtf(eyeRadius2 - d2); // Hemisphere height at d
        const float a = atan2f(d, h);           // 0 to pi/2 from centre
        const float pa = a / (float)M_PI_2 * (float)_mapRadius;
        dx /= d;
        *ptr++ = (uint8_t)(dx * pa) - x;
      } else {
        *ptr++ = 255; // Outside the eye
      }
    }
  }
  return true;
}

bool Adafruit_Monster_Eyes::calcMap(void) {
  const int pixels = _mapRadius * _mapRadius;

  _polarAngle = (uint8_t *)eyesMalloc((size_t)pixels * 2); // Both tables
  if (!_polarAngle)
    return false;
  _polarDist = (int8_t *)&_polarAngle[pixels];

  const float mapRadius2 = (float)_mapRadius * (float)_mapRadius;
  const float iRad = screen2map(_settings.irisRadius);
  const float irisRadius2 = iRad * iRad;

  uint8_t *anglePtr = _polarAngle;
  int8_t *distPtr = _polarDist;

  for (int y = 0; y < _mapRadius; y++) {
    const float dy = (float)y + 0.5f, dy2 = dy * dy;
    for (int x = 0; x < _mapRadius; x++) {
      const float dx = (float)x + 0.5f;
      const float d2 = dx * dx + dy2;
      if (d2 > mapRadius2) {
        *anglePtr++ = 0;
        *distPtr++ = -128;
      } else {
        float angle = (float)M_PI_2 - atan2f(dy, dx); // Clockwise, 0 at top
        angle *= 512.0f / (float)M_PI;                // 0 to <256 in Q1
        *anglePtr++ = (uint8_t)angle;
        float d = sqrtf(d2);
        if (d2 > irisRadius2) { // Sclera: 0..127
          d = ((float)_mapRadius - d) / ((float)_mapRadius - iRad);
          *distPtr++ = (int8_t)(d * 127.0f);
        } else { // Iris: -1..-127
          d = (iRad - d) / iRad;
          *distPtr++ = (int8_t)(d * -127.0f) - 1;
        }
      }
    }
  }

  if (_settings.slitPupilRadius > 0) {
    for (int y = 0; y < _mapRadius; y++) {
      const float dy = (float)y + 0.5f, dy2 = dy * dy;
      for (int x = 0; x < _mapRadius; x++) {
        const float dx = (float)x + 0.5f;
        const float d2 = dx * dx + dy2;
        if (d2 > irisRadius2)
          continue;
        const float xp = (float)x + 0.5f;
        for (int i = 126; i >= 0; i--) {
          const float ratio = (float)i / 128.0f; // 0 open .. just under 1 slit
          // A point between the top of the iris and the top of the slit pupil,
          // and another between the right of the iris and the centre; find the
          // circle through both.
          const float y1 =
              iRad - (iRad - (float)_settings.slitPupilRadius) * ratio;
          const float x2 = iRad * (1.0f - ratio);
          const float xc = (x2 * x2 - y1 * y1) / (2.0f * x2);
          const float rx = x2 - xc;
          const float px = xp - xc;
          if ((px * px + dy2) <= (rx * rx)) {
            _polarDist[y * _mapRadius + x] = (int8_t)(-1 - i);
            break;
          }
        }
      }
    }
  }
  return true;
}

void Adafruit_Monster_Eyes::tablesFree(void) {
  if (_polarAngle) {
    free(_polarAngle);
    _polarAngle = NULL;
    _polarDist = NULL;
  }
  if (_displace) {
    free(_displace);
    _displace = NULL;
  }
}

bool Adafruit_Monster_Eyes::tablesInit(void) {
  tablesFree();
  _mapRadius =
      (int)((float)_settings.eyeRadius * (float)M_PI * _settings.coverage +
            0.5f);
  _mapDiameter = _mapRadius * 2;
  if (_mapRadius < 8)
    return false;
  if (!calcMap()) {
    tablesFree();
    return false;
  }
  if (!calcDisplacement()) {
    tablesFree();
    return false;
  }
  return true;
}

// The original derives the gaze radius purely in map units:
//     (mapDiameter - displaySize * pi/2) * 0.75
// So also bound it by how far the iris actually moves in SCREEN pixels, via
// map2screen(). The 0.2433 factor is calibrated so the stock 240/125/0.6 demon
// eye comes out at its original radius, leaving good configs unchanged.
void Adafruit_Monster_Eyes::gazeRadiusInit(void) {
  float r = ((float)_mapDiameter - (float)_size * (float)M_PI_2) * 0.75f;

  const float travel = 0.2433f * (float)_size; // Allowed screen-pixel travel
  float s = travel / ((float)M_PI_2 * (float)_settings.eyeRadius);
  if (s > 0.999f)
    s = 0.999f;
  const float rScreen = (float)_mapRadius * asinf(s);
  // 10% tolerance. The two formulas agree exactly only at the ratio they were
  // calibrated on (eyeRadius 125 at displaySize 240); at other sizes the stock
  // eyeRadius = size/2 + 5 lands a few percent apart.
  if (r > rScreen * 1.10f) {
    EYES_DBG("gaze radius %.1f exceeds what a %d px window can show; "
             "capping to %.1f\n",
             r, _size, rScreen);
    EYES_DBG("  (eyeRadius %d is large for eye size %d; near %d fits)\n",
             _settings.eyeRadius, _size, _size / 2 + 5);
    r = rScreen;
  }
  if (r < 1.0f)
    r = 1.0f;
  _gazeRadius = r;
  EYES_DBG("Gaze radius %.1f map px (%.1f screen px)\n", _gazeRadius,
           map2screen((int)_gazeRadius));
}

// ===========================================================================
//  STARTUP
// ===========================================================================

bool Adafruit_Monster_Eyes::begin(void) {
  if (!_display) {
    fail("no display");
    return false;
  }

  EYES_DBG("\n--- Adafruit Monster Eyes ---\n");
  EYES_DBG("%s, sys clock %lu Hz, %d eye(s)\n", EYES_PLATFORM_NAME,
           (unsigned long)eyesCpuHz(), _numEyes);
  // The number that matters for frame rate is fast internal RAM, not total
  // heap -- on chips with PSRAM those are very different figures.
  EYES_DBG("fast RAM available for eye data: %u\n",
           (unsigned)eyesLargestFreeBlock());

  // Drive mode is checked FIRST, before a DVI backend touches core1 and the
  // PIOs: writing flash and driving a display cannot overlap.
  if (_driveModeEnabled && driveModeRequested()) {
    runDriveMode(); // Never returns; reboots on eject
  }

  // STORAGE BEFORE THE DISPLAY, and this order is not negotiable on RP2.
  //
  // flash.begin() probes the chip's JEDEC ID, which means disabling XIP to
  // talk to it directly. On RP2 the filesystem shares the same flash the
  // program executes from, and PicoDVI generates video from core1 -- so doing
  // this once core1 is already running is asking for the video signal to die
  // while core0 carries on happily. The symptom is a frame counter ticking
  // over a blank monitor.
  _baseSettings = _settings;

  EYES_DBG("[1] storage\n");
  if (_storageEnabled) {
    if (storageBegin())
      loadConfig(_configFile);
  } else {
    EYES_DBG("  storage disabled; using built-in defaults\n");
  }
  finalizeSettings();

  // Display next, before the tables. On a framebuffer backend this is the
  // single largest allocation in the sketch and must not have to fight
  // fragmentation.
  EYES_DBG("[2] display\n");
  if (!_display->begin()) {
    fail("display init failed");
    return false;
  }
  _swapBytes = _display->bigEndian();
  _display->setProfile(_profile);
  EYES_DBG("Display ready, max eye %d. Free heap: %u\n", _display->maxEyeSize(),
           (unsigned)eyesFreeHeap());

  // Clamp to what the backend can actually give one eye. With two eyes on a
  // single framebuffer that is half the width, so a config asking for more
  // gets quietly reduced rather than overlapping its neighbour.
  const int maxSize = _display->maxEyeSize();
  if (_settings.displaySize <= 0)
    _settings.displaySize = maxSize;
  if (_settings.displaySize > maxSize)
    _settings.displaySize = maxSize;
  finalizeSettings(); // Re-derive coverage for the final size

  // pupilMin/pupilMax are the inverse of the irisMin/irisRange the renderer
  // wants: a larger iris fraction means a smaller pupil.
  _irisMin = 1.0f - _settings.pupilMax;
  _irisRange = _settings.pupilMax - _settings.pupilMin;

  // Build the polar and displacement maps. If a config asks for an eye too
  // large for the remaining heap, step the size down rather than failing --
  // the maps grow as mapRadius^2, so this converges quickly.
  EYES_DBG("[3] tables\n");
  const uint32_t t0 = millis();
  const bool wantTexture =
      (_settings.irisFile[0] != 0) || (_settings.scleraFile[0] != 0);
  for (;;) {
    if (!tablesInit()) {
      if (_settings.displaySize <= 96) {
        fail("cannot allocate eye tables even at minimum size");
        return false;
      }
      _settings.displaySize -= 16;
      _settings.eyeRadius = 0; // Re-derive proportionally
      _settings.irisRadius = 0;
      finalizeSettings();
      EYES_DBG("Not enough RAM for tables; retrying at eye size %d\n",
               _settings.displaySize);
      continue;
    }
    const uint32_t left = eyesLargestFreeBlock();
    if (wantTexture && (_settings.displaySize > 96) &&
        (left < (uint32_t)(MONSTER_EYES_HEAP_RESERVE +
                           MONSTER_EYES_MIN_TEXTURE_BUDGET))) {
      tablesFree();
      _settings.displaySize -= 16;
      _settings.eyeRadius = 0;
      _settings.irisRadius = 0;
      finalizeSettings();
      EYES_DBG("Only %u free after tables; retrying at eye size %d to leave "
               "room for a texture\n",
               (unsigned)left, _settings.displaySize);
      continue;
    }
    break;
  }
  EYES_DBG("Tables built in %lu ms (size %d, mapRadius %d). Free heap: %u\n",
           (unsigned long)(millis() - t0), _settings.displaySize, _mapRadius,
           (unsigned)eyesFreeHeap());

  // Tell the backend the eye size BEFORE textures claim the heap: this is
  // where stripe buffers are allocated, and if they were requested afterwards
  // the texture loader could starve them, leaving a running frame counter and
  // a blank screen.
  finalizeSettings(true); // Idempotent; this is the one that reports
  _size = _settings.displaySize;
  _half = _size / 2;
  gazeRadiusInit();
  if (!_display->setEyeSize(_size)) {
    fail("display could not allocate its buffers");
    return false;
  }

  if (_selfTest)
    _display->selfTest();
  _display->clear(_settings.eyelidColor);

  // Whatever is left, minus a reserve, is the texture budget.
  const uint32_t freeHeap = eyesLargestFreeBlock();
  const uint32_t texBudget = (freeHeap > MONSTER_EYES_HEAP_RESERVE)
                                 ? (freeHeap - MONSTER_EYES_HEAP_RESERVE)
                                 : 0;
  EYES_DBG("[4] media\n");
  if (!mediaLoad(_settings.displaySize, texBudget)) {
    fail("eyelid table allocation failed");
    return false;
  }

  if (_storageEnabled && !_storageKeep)
    storageEnd();

  EYES_DBG("Running. Free heap: %u\n", (unsigned)eyesFreeHeap());

  for (uint8_t e = 0; e < _numEyes; e++) {
    _eye[e].irisSpin = -1024.0f * _variant[e].irisSpin;
    _eye[e].irisStartAngle = _variant[e].irisStartAngle;
    _eye[e].irisAngle = _eye[e].irisStartAngle;
    _eye[e].scleraSpin = -1024.0f * _variant[e].scleraSpin;
    _eye[e].scleraStartAngle = _variant[e].scleraStartAngle;
    _eye[e].scleraAngle = _eye[e].scleraStartAngle;
    _eye[e].blinkState = NOBLINK;
    _eye[e].blinkDuration = 0;
    _eye[e].blinkStartTime = 0;
    _eye[e].blinkFactor = 0.0f;
    _eye[e].pupilFactor = 0.5f;
    _eye[e].upperLidFactor = 1.0f;
    _eye[e].lowerLidFactor = 1.0f;
    _eye[e].eyeX = _eye[e].eyeY = (float)_mapRadius;
  }

  _eyeOldX = _eyeNewX = _eyeOldY = _eyeNewY = (float)_mapRadius;
  _frameEyeX = _frameEyeY = (float)_mapRadius;

  randomSeed(micros());
  // The self-test pushes columns, which accumulates into the profiling
  // counter. Clear it.
  _display->busyMicros = 0;
  _lastRateReport = micros();
  _frames = 0;
  _begun = true;
  return true;
}

// ===========================================================================
//  SWITCHING EYES AT RUN TIME
// ===========================================================================

bool Adafruit_Monster_Eyes::loadEye(const char *path) {
  if (!path)
    path = _configFile;

  // Before begin() there is nothing to replace; just remember the choice.
  if (!_begun) {
    _configFile = path;
    return true;
  }
  if (!_storageEnabled) {
    fail("loadEye: storage is disabled");
    return false;
  }

  EYES_DBG("\n--- switching to %s ---\n", path);
  freeMedia();

  const EyesSettings prev = _settings;
  EyesVariant prevVar[MONSTER_EYES_MAX_EYES];
  for (uint8_t e = 0; e < _numEyes; e++)
    prevVar[e] = _variant[e];
  const int prevSize = _settings.displaySize;

  if (!storageMounted()) {
    EYES_DBG("loadEye: remounting the filesystem. Set keepStorageMounted(true)"
             " before begin() to avoid this.\n");
    if (!storageBegin()) {
      fail("loadEye: filesystem would not mount");
      mediaLoad(_size, 0); // Put the old eye's fallbacks back
      return false;
    }
  }

  _settings = _baseSettings;
  seedVariants();

  const bool read = loadConfig(path);
  if (read) {
    _configFile = path;
  } else {
    _settings = prev;
    for (uint8_t e = 0; e < _numEyes; e++)
      _variant[e] = prevVar[e];
  }

  finalizeSettings();
  const int maxSize = _display->maxEyeSize();
  if (_settings.displaySize <= 0)
    _settings.displaySize = maxSize;
  if (_settings.displaySize > maxSize)
    _settings.displaySize = maxSize;
  finalizeSettings(true); // The one that reports, as in begin()

  _irisMin = 1.0f - _settings.pupilMax;
  _irisRange = _settings.pupilMax - _settings.pupilMin;
  if (_irisValue < _irisMin)
    _irisValue = _irisMin;
  else if (_irisValue > _irisMin + _irisRange)
    _irisValue = _irisMin + _irisRange;

  const bool geometryMoved =
      (_settings.displaySize != prevSize) ||
      (_settings.eyeRadius != prev.eyeRadius) ||
      (_settings.irisRadius != prev.irisRadius) ||
      (_settings.slitPupilRadius != prev.slitPupilRadius) ||
      (_settings.coverage != prev.coverage);

  if (geometryMoved) {
    EYES_DBG("Geometry changed; rebuilding tables\n");
    tablesFree();
    if (!tablesInit()) {
      EYES_ERR("loadEye: no room for the new tables; keeping the old eye\n");
      _settings = prev;
      tablesFree();
      if (!tablesInit()) {
        fail("loadEye: lost the eye tables and could not rebuild them");
        return false;
      }
    }

    if (_settings.displaySize != prevSize) {
      _size = _settings.displaySize;
      _half = _size / 2;
      if (!_display->setEyeSize(_size)) {
        fail("loadEye: display could not resize its buffers");
        return false;
      }
    }
    gazeRadiusInit();

    _eyeOldX = _eyeNewX = _eyeOldY = _eyeNewY = (float)_mapRadius;
    _frameEyeX = _frameEyeY = (float)_mapRadius;
    _eyeInMotion = false;
  }

  for (uint8_t e = 0; e < _numEyes; e++) {
    _eye[e].irisSpin = -1024.0f * _variant[e].irisSpin;
    _eye[e].irisStartAngle = _variant[e].irisStartAngle;
    _eye[e].scleraSpin = -1024.0f * _variant[e].scleraSpin;
    _eye[e].scleraStartAngle = _variant[e].scleraStartAngle;
  }

  _display->clear(_settings.eyelidColor);

  const uint32_t freeHeap = eyesLargestFreeBlock();
  const uint32_t texBudget = (freeHeap > MONSTER_EYES_HEAP_RESERVE)
                                 ? (freeHeap - MONSTER_EYES_HEAP_RESERVE)
                                 : 0;
  const bool loaded = mediaLoad(_size, texBudget);

  if (!_storageKeep)
    storageEnd();

  if (!loaded) {
    fail("loadEye: eyelid table allocation failed");
    return false;
  }
  if (!read) {
    fail("loadEye: could not read the configuration file");
    return false;
  }

  EYES_DBG("Now showing %s. Free heap: %u\n", path, (unsigned)eyesFreeHeap());
  return true;
}

// ===========================================================================
//  ONCE-PER-FRAME ANIMATION
// ===========================================================================

uint32_t Adafruit_Monster_Eyes::eyeMillis(void) const {
  // Iris and sclera rotation are computed from absolute time, so a board
  // following another one would otherwise sit at a fixed phase offset.
  // Standalone builds pay nothing -- the offset is zero and folds away.
  return (uint32_t)((int32_t)millis() + _clockOffset);
}

void Adafruit_Monster_Eyes::updateGaze(uint32_t t) {
  const int32_t dt = t - _eyeMoveStartTime;

  if (_eyeInMotion) {
    if (dt >= _eyeMoveDuration) { // Destination reached
      _eyeInMotion = false;
      const uint32_t limit = min((uint32_t)1000000, _settings.gazeMax);
      _eyeMoveDuration = random(35000, limit); // Hold before next microsaccade
      if (!_saccadeInterval) {
        _lastSaccadeStop = t;
        _saccadeInterval = random(_eyeMoveDuration, _settings.gazeMax);
      }
      _eyeMoveStartTime = t;
      _frameEyeX = _eyeOldX = _eyeNewX;
      _frameEyeY = _eyeOldY = _eyeNewY;
    } else { // Interpolate, ease in/out
      float e = (float)dt / (float)_eyeMoveDuration;
      e = 3.0f * e * e - 2.0f * e * e * e;
      _frameEyeX = _eyeOldX + (_eyeNewX - _eyeOldX) * e;
      _frameEyeY = _eyeOldY + (_eyeNewY - _eyeOldY) * e;
    }
  } else {
    _frameEyeX = _eyeOldX;
    _frameEyeY = _eyeOldY;
    if (dt > _eyeMoveDuration) {
      const float rFull = _gazeRadius;

      if ((t - _lastSaccadeStop) > (uint32_t)_saccadeInterval) {
        // Full saccade: anywhere in the disc.
        _eyeNewX = random(-rFull, rFull);
        const float h2 = rFull * rFull - _eyeNewX * _eyeNewX;
        const float h = (h2 > 0.0f) ? sqrtf(h2) : 0.0f;
        _eyeNewY = random(-h, h);
        _eyeMoveDuration = random(83000, 166000);
        _saccadeInterval = 0;
      } else {
        float rMicro = rFull * (0.07f / 0.75f);
        if (rMicro < 1.0f)
          rMicro = 1.0f;
        const float dx = random(-rMicro, rMicro);
        const float h2 = rMicro * rMicro - dx * dx;
        const float h = (h2 > 0.0f) ? sqrtf(h2) : 0.0f;
        _eyeNewX = _frameEyeX - _mapRadius + dx;
        _eyeNewY = _frameEyeY - _mapRadius + random(-h, h);
        _eyeMoveDuration = random(7000, 25000);
      }

      // Keep the gaze inside the disc.
      const float d2 = _eyeNewX * _eyeNewX + _eyeNewY * _eyeNewY;
      if (d2 > (rFull * rFull)) {
        const float k = rFull / sqrtf(d2);
        _eyeNewX *= k;
        _eyeNewY *= k;
      }

      _eyeNewX += _mapRadius; // Into map space
      _eyeNewY += _mapRadius;
      _eyeMoveStartTime = t;
      _eyeInMotion = true;
    }
  }
}

void Adafruit_Monster_Eyes::updateIris(void) {
  float n, sum = 0.5f;
  for (uint16_t i = 0; i < IRIS_LEVELS; i++) {
    uint16_t iexp = 1 << (i + 1);
    const uint16_t imask = iexp - 1;
    const uint16_t ibits = _irisFrame & imask;
    if (ibits) {
      const float weight = (float)ibits / (float)iexp;
      n = _irisPrev[i] * (1.0f - weight) + _irisNext[i] * weight;
    } else {
      n = _irisNext[i];
      _irisPrev[i] = _irisNext[i];
      _irisNext[i] = -0.5f + ((float)random(1000) / 999.0f);
    }
    iexp = 1 << (IRIS_LEVELS - i);
    sum += n / (float)iexp;
  }
  _irisValue = _irisMin + (sum * _irisRange);
  if ((++_irisFrame) >= (1 << IRIS_LEVELS))
    _irisFrame = 0;
}

void Adafruit_Monster_Eyes::updateBlinks(uint32_t t) {
  if ((t - _timeOfLastBlink) >= _timeToNextBlink) {
    _timeOfLastBlink = t;
    const uint32_t d = random(36000, 72000);
    for (uint8_t e = 0; e < _numEyes; e++) {
      if (_eye[e].blinkState == NOBLINK) {
        _eye[e].blinkState = ENBLINK;
        _eye[e].blinkStartTime = t;
        _eye[e].blinkDuration = d;
      }
    }
    _timeToNextBlink = d * 3 + random(4000000);
  }
}

void Adafruit_Monster_Eyes::updateEye(uint8_t e, uint32_t t) {
  EyeState &E = _eye[e];

  if (_numEyes > 1) {
    E.eyeX = _frameEyeX + ((e & 1) ? _settings.fixate : -_settings.fixate);
  } else if (_sideSet) {
    // Toe in the way this board's eye would in a two-eye build: eye 0 (right)
    // leans one way, eye 1 (left) the other.
    E.eyeX = _frameEyeX + (_sideRight ? -_settings.fixate : _settings.fixate);
  } else {
    E.eyeX = _frameEyeX;
  }
  E.eyeY = _frameEyeY;
  E.pupilFactor = _irisValue;

  float uq, lq;
  if (_settings.tracking) {
    int ix = (int)map2screen((int)((float)_mapRadius - E.eyeX)) + _half;
    int iy = (int)map2screen((int)((float)_mapRadius - E.eyeY)) + _half;
    iy += (int)(_settings.irisRadius * _settings.trackFactor);
    if (_variant[e].eyelidMirror)
      ix = _size - 1 - ix;
    if (ix < 0)
      ix = 0;
    else if (ix > _size - 1)
      ix = _size - 1;
    if (iy > _upperOpen[ix])
      uq = 1.0f;
    else if (iy < _upperClosed[ix])
      uq = 0.0f;
    else
      uq = (float)(iy - _upperClosed[ix]) /
           (float)(_upperOpen[ix] - _upperClosed[ix]);
    lq = 1.0f - uq;
  } else {
    uq = lq = 1.0f; // Fully open when not blinking
  }
  E.upperLidFactor = (E.upperLidFactor * 0.6f) + (uq * 0.4f);
  E.lowerLidFactor = (E.lowerLidFactor * 0.6f) + (lq * 0.4f);

  if (_blinkExternal) {
    // Take the phase from whoever owns it. The local state machine below still
    // runs, so if control is released mid-blink the eye carries on from where
    // it is.
    E.blinkFactor = _blinkForced;
  }
  if (E.blinkState) {
    if ((t - E.blinkStartTime) >= E.blinkDuration) {
      if (++E.blinkState > DEBLINK) {
        E.blinkState = NOBLINK;
        if (!_blinkExternal)
          E.blinkFactor = 0.0f;
      } else {
        E.blinkDuration *= 2; // Opening is half the speed of closing
        E.blinkStartTime = t;
        if (!_blinkExternal)
          E.blinkFactor = 1.0f;
      }
    } else if (!_blinkExternal) {
      E.blinkFactor = (float)(t - E.blinkStartTime) / (float)E.blinkDuration;
      if (E.blinkState == DEBLINK)
        E.blinkFactor = 1.0f - E.blinkFactor;
    }
  }

  // Cast through int32_t, NOT straight to uint16_t. Once irisSpin * mins goes
  // negative, a direct float->unsigned conversion is undefined behaviour, and
  // ARM's __aeabi_f2uiz saturates it to 0 -- which pins the iris angle at zero
  // and the iris stops spinning. Going via a signed int wraps correctly.
  const float mins = (float)eyeMillis() / 60000.0f;
  E.irisAngle =
      (uint16_t)(int32_t)((float)E.irisStartAngle + E.irisSpin * mins + 0.5f);
  E.scleraAngle = (uint16_t)(int32_t)((float)E.scleraStartAngle +
                                      E.scleraSpin * mins + 0.5f);
}

// ===========================================================================
//  LIVE CONTROL
// ===========================================================================

void Adafruit_Monster_Eyes::setPupil(float dilation) {
  if (dilation < 0.0f)
    dilation = 0.0f;
  else if (dilation > 1.0f)
    dilation = 1.0f;
  // Internally this is an IRIS fraction: bigger iris, smaller pupil.
  _irisValue = _irisMin + (1.0f - dilation) * _irisRange;
  _pupilExternal = true;
}

float Adafruit_Monster_Eyes::pupil(void) const {
  if (_irisRange <= 0.0f)
    return 0.5f;
  return 1.0f - ((_irisValue - _irisMin) / _irisRange);
}

void Adafruit_Monster_Eyes::setIrisFraction(float f) {
  _irisValue = f;
  _pupilExternal = true;
}

void Adafruit_Monster_Eyes::setGaze(float x, float y) {
  // Scale into the reachable disc rather than clipping to a square, so a
  // corner request still points as far that way as the geometry allows.
  const float d2 = x * x + y * y;
  if (d2 > 1.0f) {
    const float k = 1.0f / sqrtf(d2);
    x *= k;
    y *= k;
  }
  _frameEyeX = (float)_mapRadius + x * _gazeRadius;
  _frameEyeY = (float)_mapRadius + y * _gazeRadius;
  _gazeExternal = true;
}

void Adafruit_Monster_Eyes::setGazeMap(float x, float y) {
  _frameEyeX = x;
  _frameEyeY = y;
  _gazeExternal = true;
}

float Adafruit_Monster_Eyes::gazeX(void) const {
  if (_gazeRadius <= 0.0f)
    return 0.0f;
  return (_frameEyeX - (float)_mapRadius) / _gazeRadius;
}

float Adafruit_Monster_Eyes::gazeY(void) const {
  if (_gazeRadius <= 0.0f)
    return 0.0f;
  return (_frameEyeY - (float)_mapRadius) / _gazeRadius;
}

void Adafruit_Monster_Eyes::blink(void) {
  const uint32_t t = micros();
  const uint32_t d = random(36000, 72000);
  for (uint8_t e = 0; e < _numEyes; e++) {
    if (_eye[e].blinkState == NOBLINK) {
      _eye[e].blinkState = ENBLINK;
      _eye[e].blinkStartTime = t;
      _eye[e].blinkDuration = d;
    }
  }
  _timeOfLastBlink = t;
}

void Adafruit_Monster_Eyes::setBlink(float phase) {
  if (phase < 0.0f)
    phase = 0.0f;
  else if (phase > 1.0f)
    phase = 1.0f;
  _blinkForced = phase;
  _blinkExternal = true;
}

float Adafruit_Monster_Eyes::blinkPhase(void) const {
  return _eye[0].blinkFactor;
}

// ===========================================================================
//  RENDER
// ===========================================================================

void Adafruit_Monster_Eyes::renderEye(uint8_t e) {
  EyeState &E = _eye[e];
  const int half = _half;
  const int size = _size;
  const int stride = _display->columnStride();

  const int xPositionOverMap = (int)(E.eyeX - (float)half);
  const int yPositionOverMap = (int)(E.eyeY - (float)half);

  const float upperLidFactor = (1.0f - E.blinkFactor) * E.upperLidFactor;
  const float lowerLidFactor = (1.0f - E.blinkFactor) * E.lowerLidFactor;
  const int irisH = _irisH, irisW = _irisW;
  const int scleraH = _scleraH, scleraW = _scleraW;
  const uint16_t *iris = _irisData, *sclera = _scleraData;
  const int iPupilFactor =
      (int)((float)irisH * 256.0f * (1.0f / E.pupilFactor));
  const uint16_t irisAngle = E.irisAngle;
  const uint16_t pupilColor = out16(_settings.pupilColor);
  const uint16_t backColor = out16(_settings.backColor);
  const uint16_t eyelidColor = out16(_settings.eyelidColor);
  const uint16_t irisMirror = _variant[e].irisMirror;
  const uint16_t scleraMirror = _variant[e].scleraMirror;
  const uint16_t scleraAngle = E.scleraAngle;
  const bool mirrorLids = _variant[e].eyelidMirror;
  const uint8_t *displace = _displace;
  const uint8_t *polarAngle = _polarAngle;
  const int8_t *polarDist = _polarDist;
  const int mapRadius = _mapRadius, mapDiameter = _mapDiameter;

  for (int x = 0; x < size; x++) {
    const int lidColumn = mirrorLids ? (size - 1 - x) : x;

    // Destination pointer starts at the TOP of the column and walks up-screen
    // as y increases.
    uint16_t *dst = _display->column(e, x);
    if (!dst)
      return;

    int y1 =
        (int)_lowerClosed[lidColumn] +
        (int)(0.5f + lowerLidFactor * (float)((int)_lowerOpen[lidColumn] -
                                              (int)_lowerClosed[lidColumn]));
    int y2 =
        (int)_upperClosed[lidColumn] +
        (int)(0.5f + upperLidFactor * (float)((int)_upperOpen[lidColumn] -
                                              (int)_upperClosed[lidColumn]));
    if (y1 > size - 1)
      y1 = size - 1;
    else if (y1 < 0)
      y1 = 0;
    if (y2 > size - 1)
      y2 = size - 1;
    else if (y2 < 0)
      y2 = 0;

    if (y1 >= y2) {
      // Lid closed far enough that no eye pixels show in this column
      for (int y = 0; y < size; y++, dst += stride)
        *dst = eyelidColor;
      _display->columnDone(e, x);
      continue;
    }

    // Lower eyelid
    int y = 0;
    for (; y < y1; y++, dst += stride)
      *dst = eyelidColor;

    // Displacement lookup setup for this column. Only one quadrant of the
    // table exists; sign and axis swapping cover the rest.
    const uint8_t *displaceX, *displaceY;
    int8_t xmul;
    if (x < half) {
      displaceX = &displace[(half - 1) - x];
      displaceY = &displace[((half - 1) - x) * half];
      xmul = -1;
    } else {
      displaceX = &displace[x - half];
      displaceY = &displace[(x - half) * half];
      xmul = 1;
    }

    const int xx = xPositionOverMap + x;

    for (; y <= y2; y++, dst += stride) {
      const int yy = yPositionOverMap + y;
      int doff, dx, dy;

      if (y < half) {
        doff = (half - 1) - y;
        dy = -(int)displaceY[doff];
      } else {
        doff = y - half;
        dy = (int)displaceY[doff];
      }
      dx = displaceX[doff * half];

      if (dx >= 255) { // Outside the eyeball
        *dst = eyelidColor;
        continue;
      }
      dx *= xmul;
      int mx = xx + dx;
      int my = yy + dy;

      if ((mx < 0) || (mx >= mapDiameter) || (my < 0) || (my >= mapDiameter)) {
        *dst = backColor; // Off the map
        continue;
      }

      int angle, dist, moff;
      if (my >= mapRadius) {
        if (mx >= mapRadius) { // Quadrant 1: direct
          mx -= mapRadius;
          my -= mapRadius;
          moff = my * mapRadius + mx;
          angle = polarAngle[moff];
          dist = polarDist[moff];
        } else { // Quadrant 2: rotate 90, mirror X
          mx = mapRadius - 1 - mx;
          my -= mapRadius;
          angle = polarAngle[mx * mapRadius + my] + 768;
          dist = polarDist[my * mapRadius + mx];
        }
      } else {
        if (mx < mapRadius) { // Quadrant 3: rotate 180
          mx = mapRadius - 1 - mx;
          my = mapRadius - 1 - my;
          moff = my * mapRadius + mx;
          angle = polarAngle[moff] + 512;
          dist = polarDist[moff];
        } else { // Quadrant 4: rotate 270, mirror Y
          mx -= mapRadius;
          my = mapRadius - 1 - my;
          angle = polarAngle[mx * mapRadius + my] + 256;
          dist = polarDist[my * mapRadius + mx];
        }
      }

      if (dist >= 0) { // Sclera
        const int a = ((angle + scleraAngle) & 1023) ^ scleraMirror;
        const int tx = (a * scleraW) >> 10;
        const int ty = (dist * scleraH) >> 7;
        *dst = sclera[ty * scleraW + tx];
      } else if (dist > -128) { // Iris or pupil
        const int ty = (int)(((uint32_t)(-dist * iPupilFactor)) >> 15);
        if (ty >= irisH) {
          *dst = pupilColor;
        } else {
          const int a = ((angle + irisAngle) & 1023) ^ irisMirror;
          const int tx = (a * irisW) >> 10;
          *dst = iris[ty * irisW + tx];
        }
      } else {
        *dst = backColor; // Back of eye
      }
    }

    // Upper eyelid
    for (; y < size; y++, dst += stride)
      *dst = eyelidColor;

    _display->columnDone(e, x);
  }
}

// ===========================================================================
//  FRAME
// ===========================================================================

void Adafruit_Monster_Eyes::update(void) {
  if (!_begun)
    return;
  _frameMicros = micros();

  if (_autoGaze && !_gazeExternal)
    updateGaze(_frameMicros);
  if (_autoBlink && !_blinkExternal)
    updateBlinks(_frameMicros);
  if (!_pupilExternal)
    updateIris();
}

void Adafruit_Monster_Eyes::draw(void) {
  if (!_begun)
    return;
  const uint32_t t = _frameMicros ? _frameMicros : micros();

  _display->frameBegin();
  for (uint8_t e = 0; e < _numEyes; e++) {
    updateEye(e, t);
    _display->eyeBegin(e);
    renderEye(e);
    _display->eyeEnd(e);
  }
  _display->frameEnd();

  _frames++;
  if (_profile) {
    const float frameMs = (float)(micros() - t) / 1000.0f;
    _lastTransferMs = (float)_display->busyMicros / 1000.0f;
    _display->busyMicros = 0;
    _lastRenderMs = frameMs - _lastTransferMs;
  }
  _frameMicros = 0;
}

void Adafruit_Monster_Eyes::animate(void) {
  update();
  draw();
}

float Adafruit_Monster_Eyes::frameRate(void) {
  const uint32_t now = micros();
  const uint32_t elapsed = now - _lastRateReport;
  if (elapsed == 0)
    return 0.0f;
  const float fps = (float)_frames * 1000000.0f / (float)elapsed;
  _frames = 0;
  _lastRateReport = now;
  return fps;
}
