/**
 * @file Adafruit_Monster_Eyes.ino
 * @brief Animated eyes for RP2040/RP2350 and ESP32, ported from Adafruit's
 *        M4_Eyes.
 *
 * Original: Phillip Burgess for Adafruit Industries, MIT license.
 *
 *
 * @see settings.h for everything a user configures.
 */

#include "eye.h"

#if EYE_SYNC != EYE_SYNC_OFF
#include <Wire.h>
#if EYE_SYNC == EYE_SYNC_SECONDARY
static void syncOnReceive(int n);
#endif
#endif

#include <math.h>

static int SIZE, HALF;
static float gazeRadius = 1.0f;
static void gazeRadiusInit(void);

// EYE STATE ---------------------------------------------------------------

#define NOBLINK 0
#define ENBLINK 1
#define DEBLINK 2

typedef struct {
  float irisSpin, scleraSpin; // RPM * -1024 (negative = clockwise)
  uint16_t irisStartAngle, scleraStartAngle; // 0-1023 CCW
  uint16_t irisAngle, scleraAngle;           // Current rotation

  uint8_t blinkState;
  uint32_t blinkDuration;
  uint32_t blinkStartTime;
  float blinkFactor;

  float eyeX, eyeY; // Position in map space, saved per eye to avoid tearing
  float pupilFactor;
  float upperLidFactor, lowerLidFactor;
} eyeState;

static eyeState eye[NUM_EYES];

// Shared animation state
static bool eyeInMotion = false;
static float eyeOldX, eyeOldY, eyeNewX, eyeNewY;
static uint32_t eyeMoveStartTime = 0;
static int32_t eyeMoveDuration = 0;
static uint32_t lastSaccadeStop = 0;
static int32_t saccadeInterval = 0;
static uint32_t timeOfLastBlink = 0;
static uint32_t timeToNextBlink = 0;
static float frameEyeX, frameEyeY;

// Autonomous iris scaling via fractal subdivision (no light sensor here)
#define IRIS_LEVELS 7
static float irisPrev[IRIS_LEVELS] = {0};
static float irisNext[IRIS_LEVELS] = {0};
static uint16_t irisFrame = 0;
static float irisValue = 0.5f;
static float irisMin, irisRange;

static uint32_t frames = 0;
static uint32_t lastFrameReport = 0;
static uint32_t accFrameMicros = 0, accBusyMicros = 0;

// SETUP -------------------------------------------------------------------

void setup() {
  Serial.begin(115200);

  // If the board hangs during startup, the last line printed
  // says which step died. A hard fault kills USB
#if STARTUP_GRACE_MS > 0
  delay(STARTUP_GRACE_MS);
#endif
  DBGLN("\n--- RP2 Eyes ---");
  DBG("%s, sys clock %lu Hz\n", PLATFORM_NAME, (unsigned long)platformCpuHz());
  DBG("board: %s, %d eye(s), panel %s\n", EYE_BOARD_NAME, NUM_EYES,
      (EYE_PANEL == EYE_PANEL_DVI)       ? "DVI"
      : (EYE_PANEL == EYE_PANEL_GC9A01A) ? "GC9A01A"
      : (EYE_PANEL == EYE_PANEL_ILI9341) ? "ILI9341"
                                         : "ST7789");
  // The number that matters for frame rate is fast internal RAM, not total
  // heap -- on chips with PSRAM those are very different figures.
  DBG("fast RAM available for eye data: %u\n", platformLargestFreeBlock());

  // Drive mode is checked FIRST, before DVI touches core1 or the PIOs.
  // See eye_storage.cpp for why the two modes cannot run at the same time.
#if ENABLE_BOOTSEL_DRIVE
  if (eyeStorageDriveModeRequested()) {
    eyeStorageRunDriveMode(); // Never returns; reboots on eject
  }
#endif

  DBGLN("[1] settings");
  eyeSettingsDefaults();
  DBGLN("[2] storage");
#if ENABLE_STORAGE
  if (eyeStorageBegin()) {
    eyeSettingsLoad(CONFIG_FILENAME);
  }
#else
  DBGLN("  storage disabled; using built-in defaults");
#endif
  eyeSettingsFinalize();

  // Display first. On DVI this allocates the framebuffer, the single largest
  // allocation in the sketch, which must not have to fight fragmentation.
  DBGLN("[3] display");
  if (!displayBegin()) {
    // Framebuffer allocation failed. Almost always means the eye tables or
    // something else claimed RAM first, or the resolution is too large.
    pinMode(LED_BUILTIN, OUTPUT);
    for (;;)
      digitalWrite(LED_BUILTIN, (millis() / 200) & 1);
  }
  DBG("Display ready, max eye %d. Free heap: %u\n", displayMaxEyeSize(),
      platformFreeHeap());

#if EYE_SYNC != EYE_SYNC_OFF
  // Brought up AFTER the display. PicoDVI overclocks the chip
  // when it starts, and the I2C clock divider is fixed from the peripheral
  // clock at begin() time.
#if EYE_SYNC == EYE_SYNC_PRIMARY
  EYE_SYNC_I2C_WIRE.begin(); // Controller
  EYE_SYNC_I2C_WIRE.setClock(EYE_SYNC_I2C_HZ);
  DBG("sync: PRIMARY writing to I2C 0x%02X at %ld Hz, clk %lu Hz\n",
      EYE_SYNC_I2C_ADDR, (long)EYE_SYNC_I2C_HZ, (unsigned long)platformCpuHz());
#else
  EYE_SYNC_I2C_WIRE.begin(EYE_SYNC_I2C_ADDR); // Peripheral
  EYE_SYNC_I2C_WIRE.onReceive(syncOnReceive);
  DBG("sync: SECONDARY answering I2C 0x%02X, clk %lu Hz\n", EYE_SYNC_I2C_ADDR,
      (unsigned long)platformCpuHz());
#endif
  DBG("sync: drawing the %s eye\n", EYE_SYNC_SIDE_RIGHT ? "RIGHT" : "LEFT");
#endif

#if DISPLAY_SELFTEST
  displaySelfTest();
#endif
  displayClear(0);

  // Clamp to what the backend can actually give one eye. With two eyes on a
  // single framebuffer this is half the width, so a config asking for more
  // gets quietly reduced rather than overlapping its neighbor.
  // displaySize 0 in config.eye means "fill the display".
  if (settings.displaySize <= 0)
    settings.displaySize = displayMaxEyeSize();
  if (settings.displaySize > displayMaxEyeSize())
    settings.displaySize = displayMaxEyeSize();
  eyeSettingsFinalize(); // Re-derive coverage for the final displaySize

  // pupilMin/pupilMax in the file are the inverse of the irisMin/irisRange
  // the renderer wants.
  irisMin = 1.0f - settings.pupilMax;
  irisRange = settings.pupilMax - settings.pupilMin;

  // Build the polar and displacement maps. If a config asks for an eye too
  // large for the remaining heap, step the size down rather than failing --
  // the maps grow as mapRadius^2, so this converges quickly.
  DBGLN("[4] tables");
  uint32_t t0 = millis();

  // Shrink the eye until the maps fit AND a texture still has somewhere to
  // live.
  const bool wantTexture =
      (settings.irisFile[0] != 0) || (settings.scleraFile[0] != 0);
  for (;;) {
    if (!eyeTablesInit()) {
      if (settings.displaySize <= 96) {
        Serial.println("Cannot allocate eye tables even at minimum size.");
        for (;;)
          delay(1000);
      }
      settings.displaySize -= 16;
      settings.eyeRadius = 0; // Re-derive proportionally
      settings.irisRadius = 0;
      eyeSettingsFinalize();
      DBG("Not enough RAM for tables; retrying at displaySize %d\n",
          settings.displaySize);
      continue;
    }
    uint32_t left = platformLargestFreeBlock();
    if (wantTexture && (settings.displaySize > 96) &&
        (left < (uint32_t)(HEAP_RESERVE + MIN_TEXTURE_BUDGET))) {
      eyeTablesFree();
      settings.displaySize -= 16;
      settings.eyeRadius = 0;
      settings.irisRadius = 0;
      eyeSettingsFinalize();
      DBG("Only %u free after tables; retrying at displaySize %d "
          "to leave room for a texture\n",
          (unsigned)left, settings.displaySize);
      continue;
    }
    break;
  }
  DBG("Tables built in %lu ms (size %d, mapRadius %d). Free heap: %u\n",
      millis() - t0, settings.displaySize, mapRadius, platformFreeHeap());
  (void)t0;

  // Tell the backend the eye size BEFORE textures claim the heap
  SIZE = settings.displaySize;
  HALF = SIZE / 2;
  gazeRadiusInit();
  displaySetEyeSize(SIZE);
#if DISPLAY_PATHTEST
  displayPathTest(SIZE);
#endif
  displayClear(settings.eyelidColor);

  // Whatever is left, minus a reserve, is the texture budget.
  uint32_t freeHeap = platformLargestFreeBlock();
  uint32_t texBudget =
      (freeHeap > HEAP_RESERVE) ? (freeHeap - HEAP_RESERVE) : 0;
  DBGLN("[5] media");
  if (!eyeMediaLoad(settings.displaySize, texBudget)) {
    Serial.println("Eyelid table allocation failed.");
    for (;;)
      delay(1000);
  }

  // Nothing else reads the filesystem; let go of it so flash stays quiet.
#if ENABLE_STORAGE
  eyeStorageEnd();
#endif

  DBG("Running. Free heap: %u\n", platformFreeHeap());
  // The self-test and path test both push columns, which accumulates into the
  // profiling counter. Clear it.
  displayBusyMicros = 0;
  lastFrameReport = micros();

  for (uint8_t e = 0; e < NUM_EYES; e++) {
    eye[e].irisSpin = -1024.0f * eyeVariant[e].irisSpin;
    eye[e].irisStartAngle = eyeVariant[e].irisStartAngle;
    eye[e].irisAngle = eye[e].irisStartAngle;
    eye[e].scleraSpin = -1024.0f * eyeVariant[e].scleraSpin;
    eye[e].scleraStartAngle = eyeVariant[e].scleraStartAngle;
    eye[e].scleraAngle = eye[e].scleraStartAngle;
    eye[e].blinkState = NOBLINK;
    eye[e].blinkFactor = 0.0f;
    eye[e].pupilFactor = 0.5f;
    eye[e].upperLidFactor = 1.0f;
    eye[e].lowerLidFactor = 1.0f;
    eye[e].eyeX = eye[e].eyeY = (float)mapRadius;
  }

  eyeOldX = eyeNewX = eyeOldY = eyeNewY = (float)mapRadius;
  frameEyeX = frameEyeY = (float)mapRadius;

  randomSeed(micros());
}

// ONCE-PER-FRAME ANIMATION ------------------------------------------------

// The original derives the gaze radius purely in map units:
//     (mapDiameter - displaySize * pi/2) * 0.75
// So also bound it by how far the iris actually moves in SCREEN pixels, via
// map2screen(). The 0.2433 factor is calibrated so the stock 240/125/0.6
// demon eye comes out at its original radius, leaving good configs unchanged.
static void gazeRadiusInit(void) {
  float r = ((float)mapDiameter - (float)SIZE * (float)M_PI_2) * 0.75f;

  const float travel = 0.2433f * (float)SIZE; // Allowed screen-pixel travel
  float s = travel / ((float)M_PI_2 * (float)settings.eyeRadius);
  if (s > 0.999f)
    s = 0.999f;
  const float rScreen = (float)mapRadius * asinf(s);
  // 10% tolerance. The two formulas agree exactly only at the ratio they were
  // calibrated on (eyeRadius 125 at displaySize 240); at other sizes the
  // stock eyeRadius = size/2 + 5 lands a few percent apart
  if (r > rScreen * 1.10f) {
    DBG("gaze radius %.1f exceeds what a %d px window can show; "
        "capping to %.1f\n",
        r, SIZE, rScreen);
    DBG("  (eyeRadius %d is large for displaySize %d; near %d fits)\n",
        settings.eyeRadius, SIZE, SIZE / 2 + 5);
    r = rScreen;
  }
  if (r < 1.0f)
    r = 1.0f;
  gazeRadius = r;
  DBG("Gaze radius %.1f map px (%.1f screen px)\n", gazeRadius,
      map2screen((int)gazeRadius));
}

#if EYE_SYNC != EYE_SYNC_OFF

/** @brief XOR checksum over a packet's leading bytes. */
static uint8_t syncChecksum(const uint8_t *p) {
  uint8_t x = 0;
  for (size_t i = 0; i < sizeof(EyeSyncPacket) - 1; i++)
    x ^= p[i];
  return x;
}

static uint32_t syncSent = 0;          ///< Packets transmitted this second
static volatile uint32_t syncGood = 0; ///< Valid packets received this second
static volatile uint32_t syncBad = 0;  ///< Packets dropped on a bad checksum
static volatile uint32_t syncBytes =
    0; ///< Raw bytes seen on the link this second
#if EYE_SYNC == EYE_SYNC_SECONDARY
/// Packet latched by the I2C interrupt, waiting to be consumed.
static volatile uint8_t i2cBuf[sizeof(EyeSyncPacket)];
static volatile bool i2cHave = false; ///< A packet is waiting in i2cBuf

/**
 * @brief I2C receive interrupt: latch one packet and return.
 *
 * Runs in interrupt context, so it does nothing but copy. Validation and the
 * animation update happen in syncReceive() on the main loop.
 *
 * @param n Bytes the controller wrote.
 */
static void syncOnReceive(int n) {
  if (n != (int)sizeof(EyeSyncPacket)) {
    while (EYE_SYNC_I2C_WIRE.available())
      EYE_SYNC_I2C_WIRE.read();
    syncBad++;
    return;
  }
  for (size_t i = 0; i < sizeof(EyeSyncPacket); i++)
    i2cBuf[i] = (uint8_t)EYE_SYNC_I2C_WIRE.read();
  syncBytes += (uint32_t)n;
  i2cHave = true;
}
#endif

static int32_t syncClockOffset = 0;  ///< Primary millis() minus ours
static float syncBlinkFactor = 0.0f; ///< Blink phase from the link
static bool syncLive = false;        ///< Are packets currently arriving?
#endif

/**
 * @brief Milliseconds on the primary's clock.
 *
 * Iris and sclera rotation are computed from absolute time, so a secondary
 * that used its own uptime would sit at a fixed phase offset. Standalone
 * builds pay nothing -- the offset is zero and folds away.
 */
static uint32_t eyeMillis(void) {
#if EYE_SYNC == EYE_SYNC_SECONDARY
  return (uint32_t)((int32_t)millis() + syncClockOffset);
#else
  return millis();
#endif
}

#if EYE_SYNC == EYE_SYNC_PRIMARY
/** @brief Broadcast this frame's shared state. */
static void syncSend(void) {
  EyeSyncPacket p;
  p.magic = EYE_SYNC_MAGIC;
  p.eyeX = (int16_t)lroundf(frameEyeX);
  p.eyeY = (int16_t)lroundf(frameEyeY);
  p.iris = (uint16_t)(constrain(irisValue, 0.0f, 1.0f) * 65535.0f);
  p.blink = (uint8_t)(constrain(eye[0].blinkFactor, 0.0f, 1.0f) * 255.0f);
  p.ms = millis();
  p.sum = syncChecksum((const uint8_t *)&p);
  EYE_SYNC_I2C_WIRE.beginTransmission(EYE_SYNC_I2C_ADDR);
  EYE_SYNC_I2C_WIRE.write((const uint8_t *)&p, sizeof(p));
  // Non-zero means the secondary did not acknowledge
  if (EYE_SYNC_I2C_WIRE.endTransmission() != 0) {
    syncBad++;
  } else {
    syncSent++;
  }
}
#endif

#if EYE_SYNC == EYE_SYNC_SECONDARY
/**
 * @brief Consume any waiting packets, keeping only the most recent.
 *
 * Reading everything available rather than one packet per frame means a
 * secondary rendering slower than the primary tracks the latest state instead
 * of falling progressively further behind.
 *
 * @return true if a good packet arrived recently enough to trust.
 */
/**
 * @brief Adopt a validated packet's state.
 * @param p Packet already checked for magic and checksum.
 */
static void syncApply(const EyeSyncPacket &p) {
  frameEyeX = (float)p.eyeX;
  frameEyeY = (float)p.eyeY;
  irisValue = (float)p.iris / 65535.0f;
  syncBlinkFactor = (float)p.blink / 255.0f;
  syncClockOffset = (int32_t)p.ms - (int32_t)millis();
}

static bool syncReceive(void) {
  static uint32_t lastGood = 0;

  // The interrupt latches a whole packet or nothing
  if (i2cHave) {
    EyeSyncPacket p;
    noInterrupts();
    memcpy(&p, (const void *)i2cBuf, sizeof(p));
    i2cHave = false;
    interrupts();
    if (p.magic == EYE_SYNC_MAGIC &&
        p.sum == syncChecksum((const uint8_t *)&p)) {
      syncGood++;
      syncApply(p);
      lastGood = millis();
    } else {
      syncBad++;
    }
  }
  return (lastGood != 0) && ((millis() - lastGood) < EYE_SYNC_TIMEOUT_MS);
}
#endif

static void updateGaze(uint32_t t) {
  int32_t dt = t - eyeMoveStartTime;

  if (eyeInMotion) {
    if (dt >= eyeMoveDuration) { // Destination reached
      eyeInMotion = false;
      uint32_t limit = min((uint32_t)1000000, settings.gazeMax);
      eyeMoveDuration = random(35000, limit); // Hold before next microsaccade
      if (!saccadeInterval) {
        lastSaccadeStop = t;
        saccadeInterval = random(eyeMoveDuration, settings.gazeMax);
      }
      eyeMoveStartTime = t;
      frameEyeX = eyeOldX = eyeNewX;
      frameEyeY = eyeOldY = eyeNewY;
    } else { // Interpolate, ease in/out
      float e = (float)dt / (float)eyeMoveDuration;
      e = 3.0f * e * e - 2.0f * e * e * e;
      frameEyeX = eyeOldX + (eyeNewX - eyeOldX) * e;
      frameEyeY = eyeOldY + (eyeNewY - eyeOldY) * e;
    }
  } else {
    frameEyeX = eyeOldX;
    frameEyeY = eyeOldY;
    if (dt > eyeMoveDuration) {
      const float rFull = gazeRadius;

      if ((t - lastSaccadeStop) > (uint32_t)saccadeInterval) {
        // Full saccade: anywhere in the disc.
        eyeNewX = random(-rFull, rFull);
        float h2 = rFull * rFull - eyeNewX * eyeNewX;
        float h = (h2 > 0.0f) ? sqrtf(h2) : 0.0f;
        eyeNewY = random(-h, h);
        eyeMoveDuration = random(83000, 166000);
        saccadeInterval = 0;
      } else {
        float rMicro = rFull * (0.07f / 0.75f);
        if (rMicro < 1.0f)
          rMicro = 1.0f;
        float dx = random(-rMicro, rMicro);
        float h2 = rMicro * rMicro - dx * dx;
        float h = (h2 > 0.0f) ? sqrtf(h2) : 0.0f;
        eyeNewX = frameEyeX - mapRadius + dx;
        eyeNewY = frameEyeY - mapRadius + random(-h, h);
        eyeMoveDuration = random(7000, 25000);
      }

      // Keep the gaze inside the disc
      float d2 = eyeNewX * eyeNewX + eyeNewY * eyeNewY;
      if (d2 > (rFull * rFull)) {
        float k = rFull / sqrtf(d2);
        eyeNewX *= k;
        eyeNewY *= k;
      }

      eyeNewX += mapRadius; // Into map space
      eyeNewY += mapRadius;
      eyeMoveStartTime = t;
      eyeInMotion = true;
    }
  }
}

static void updateIris(void) {
  float n, sum = 0.5f;
  for (uint16_t i = 0; i < IRIS_LEVELS; i++) {
    uint16_t iexp = 1 << (i + 1);
    uint16_t imask = iexp - 1;
    uint16_t ibits = irisFrame & imask;
    if (ibits) {
      float weight = (float)ibits / (float)iexp;
      n = irisPrev[i] * (1.0f - weight) + irisNext[i] * weight;
    } else {
      n = irisNext[i];
      irisPrev[i] = irisNext[i];
      irisNext[i] = -0.5f + ((float)random(1000) / 999.0f);
    }
    iexp = 1 << (IRIS_LEVELS - i);
    sum += n / (float)iexp;
  }
  irisValue = irisMin + (sum * irisRange);
  if ((++irisFrame) >= (1 << IRIS_LEVELS))
    irisFrame = 0;
}

static void updateBlinks(uint32_t t) {
  if ((t - timeOfLastBlink) >= timeToNextBlink) {
    timeOfLastBlink = t;
    uint32_t d = random(36000, 72000);
    for (uint8_t e = 0; e < NUM_EYES; e++) {
      if (eye[e].blinkState == NOBLINK) {
        eye[e].blinkState = ENBLINK;
        eye[e].blinkStartTime = t;
        eye[e].blinkDuration = d;
      }
    }
    timeToNextBlink = d * 3 + random(4000000);
  }
}

static void updateEye(uint8_t e, uint32_t t) {
  eyeState &E = eye[e];

#if NUM_EYES > 1
  E.eyeX = frameEyeX + ((e & 1) ? EYE_FIXATE : -EYE_FIXATE);
#elif EYE_SYNC != EYE_SYNC_OFF
  // Toe in the way this board's eye would in a two-eye build: eye 0 (right)
  // leans one way, eye 1 (left) the other.
  E.eyeX = frameEyeX + (EYE_SYNC_SIDE_RIGHT ? -EYE_FIXATE : EYE_FIXATE);
#else
  E.eyeX = frameEyeX;
#endif
  E.eyeY = frameEyeY;
  E.pupilFactor = irisValue;

  float uq, lq;
  if (settings.tracking) {
    int ix = (int)map2screen((float)mapRadius - E.eyeX) + HALF;
    int iy = (int)map2screen((float)mapRadius - E.eyeY) + HALF;
    iy += (int)(settings.irisRadius * settings.trackFactor);
    if (eyeVariant[e].eyelidMirror)
      ix = SIZE - 1 - ix;
    if (ix < 0)
      ix = 0;
    else if (ix > SIZE - 1)
      ix = SIZE - 1;
    if (iy > upperOpen[ix])
      uq = 1.0f;
    else if (iy < upperClosed[ix])
      uq = 0.0f;
    else
      uq = (float)(iy - upperClosed[ix]) /
           (float)(upperOpen[ix] - upperClosed[ix]);
    lq = 1.0f - uq;
  } else {
    uq = lq = 1.0f; // Fully open when not blinking
  }
  E.upperLidFactor = (E.upperLidFactor * 0.6f) + (uq * 0.4f);
  E.lowerLidFactor = (E.lowerLidFactor * 0.6f) + (lq * 0.4f);

#if EYE_SYNC == EYE_SYNC_SECONDARY
  if (syncLive) {
    // Take the phase from the link. The local state machine below still runs,
    // so if the link drops mid-blink the eye carries on from where it is.
    E.blinkFactor = syncBlinkFactor;
  }
#endif
  if (E.blinkState) {
    if ((t - E.blinkStartTime) >= E.blinkDuration) {
      if (++E.blinkState > DEBLINK) {
        E.blinkState = NOBLINK;
        E.blinkFactor = 0.0f;
      } else {
        E.blinkDuration *= 2; // Opening is half the speed of closing
        E.blinkStartTime = t;
        E.blinkFactor = 1.0f;
      }
    } else {
#if EYE_SYNC == EYE_SYNC_SECONDARY
      if (!syncLive)
#endif
      {
        E.blinkFactor = (float)(t - E.blinkStartTime) / (float)E.blinkDuration;
        if (E.blinkState == DEBLINK)
          E.blinkFactor = 1.0f - E.blinkFactor;
      }
    }
  }

  // Cast through int32_t, NOT straight to uint16_t. Once irisSpin * mins goes
  // negative, a direct float->unsigned conversion is undefined behavior, and
  // ARM's __aeabi_f2uiz saturates it to 0 -- which pins the iris angle at zero
  // and the iris stops spinning. Going via a signed int wraps correctly.
  float mins = (float)eyeMillis() / 60000.0f;
  E.irisAngle =
      (uint16_t)(int32_t)((float)E.irisStartAngle + E.irisSpin * mins + 0.5f);
  E.scleraAngle = (uint16_t)(int32_t)((float)E.scleraStartAngle +
                                      E.scleraSpin * mins + 0.5f);
}

// RENDER ------------------------------------------------------------------

static void EYE_HOT_FN(renderEye)(uint8_t e) {
  eyeState &E = eye[e];
  const int half = HALF;
  const int stride = displayColumnStride;

  const int xPositionOverMap = (int)(E.eyeX - (float)half);
  const int yPositionOverMap = (int)(E.eyeY - (float)half);

  const float upperLidFactor = (1.0f - E.blinkFactor) * E.upperLidFactor;
  const float lowerLidFactor = (1.0f - E.blinkFactor) * E.lowerLidFactor;
  const int irisH = irisHeight(), irisW = irisWidth();
  const int scleraH = scleraHeight(), scleraW = scleraWidth();
  const uint16_t *iris = irisData, *sclera = scleraData;
  const int iPupilFactor =
      (int)((float)irisH * 256.0f * (1.0f / E.pupilFactor));
  const uint16_t irisAngle = E.irisAngle;
  const uint16_t pupilColor = OUT16(settings.pupilColor);
  const uint16_t backColor = OUT16(settings.backColor);
  const uint16_t eyelidColor = OUT16(settings.eyelidColor);
  const uint16_t irisMirror = eyeVariant[e].irisMirror;
  const uint16_t scleraMirror = eyeVariant[e].scleraMirror;
  const uint16_t scleraAngle = E.scleraAngle;
  const bool mirrorLids = eyeVariant[e].eyelidMirror;

  for (int x = 0; x < SIZE; x++) {
    const int lidColumn = mirrorLids ? (SIZE - 1 - x) : x;

    // Destination pointer starts at the TOP of the column and walks up-screen
    // as y increases.
    uint16_t *dst = displayColumn(e, x);

    int y1 =
        (int)lowerClosed[lidColumn] +
        (int)(0.5f + lowerLidFactor * (float)((int)lowerOpen[lidColumn] -
                                              (int)lowerClosed[lidColumn]));
    int y2 =
        (int)upperClosed[lidColumn] +
        (int)(0.5f + upperLidFactor * (float)((int)upperOpen[lidColumn] -
                                              (int)upperClosed[lidColumn]));
    if (y1 > SIZE - 1)
      y1 = SIZE - 1;
    else if (y1 < 0)
      y1 = 0;
    if (y2 > SIZE - 1)
      y2 = SIZE - 1;
    else if (y2 < 0)
      y2 = 0;

    if (y1 >= y2) {
      // Lid closed far enough that no eye pixels show in this column
      for (int y = 0; y < SIZE; y++, dst += stride)
        *dst = eyelidColor;
      displayColumnDone(e, x);
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
        int a = ((angle + scleraAngle) & 1023) ^ scleraMirror;
        int tx = (a * scleraW) >> 10;
        int ty = (dist * scleraH) >> 7;
        *dst = sclera[ty * scleraW + tx];
      } else if (dist > -128) { // Iris or pupil
        int ty = (int)(((uint32_t)(-dist * iPupilFactor)) >> 15);
        if (ty >= irisH) {
          *dst = pupilColor;
        } else {
          int a = ((angle + irisAngle) & 1023) ^ irisMirror;
          int tx = (a * irisW) >> 10;
          *dst = iris[ty * irisW + tx];
        }
      } else {
        *dst = backColor; // Back of eye
      }
    }

    // Upper eyelid
    for (; y < SIZE; y++, dst += stride)
      *dst = eyelidColor;

    displayColumnDone(e, x);
  }
}

// LOOP --------------------------------------------------------------------

void loop() {
  uint32_t t = micros();

#if EYE_SYNC == EYE_SYNC_SECONDARY
  // Animate locally only while the link is quiet, so a pulled cable leaves a
  // working eye rather than a frozen one.
  syncLive = syncReceive();
  if (!syncLive) {
    updateGaze(t);
    updateBlinks(t);
    updateIris();
  }
#else
  updateGaze(t);
  updateBlinks(t);
  updateIris();
#endif

  displayFrameBegin();
  for (uint8_t e = 0; e < NUM_EYES; e++) {
    updateEye(e, t);
    displayEyeBegin(e);
    renderEye(e);
    displayEyeEnd(e);
  }
  displayFrameEnd();

#if EYE_SYNC == EYE_SYNC_PRIMARY
  syncSend();
#endif

  frames++;
#if PROFILE_FRAME
  accFrameMicros += micros() - t;
  accBusyMicros += displayBusyMicros;
  displayBusyMicros = 0;
#endif
  if ((t - lastFrameReport) >= 1000000) {
#if PROFILE_FRAME
    float f = (float)accFrameMicros / (float)frames / 1000.0f;
    float tx = (float)accBusyMicros / (float)frames / 1000.0f;
    Serial.printf("%lu fps  frame %.1f ms = render %.1f + transfer %.1f "
                  "(%.0f%% transfer)\n",
                  frames, f, f - tx, tx, (f > 0.0f) ? (100.0f * tx / f) : 0.0f);
    accFrameMicros = accBusyMicros = 0;
#else
    Serial.printf("%lu fps\n", frames);
#endif
#if EYE_SYNC == EYE_SYNC_PRIMARY
    DBG("  sync: sent %lu packets, %lu failed\n", (unsigned long)syncSent,
        (unsigned long)syncBad);
    syncSent = syncBad = 0;
#elif EYE_SYNC == EYE_SYNC_SECONDARY
    DBG("  sync: %s, %lu good, %lu bad, %lu bytes\n",
        syncLive ? "LOCKED" : "free-running", (unsigned long)syncGood,
        (unsigned long)syncBad, (unsigned long)syncBytes);
    // Only comment when something is actually wrong. LOCKED with good packets
    // needs no advice.
    if (!syncLive) {
      if (syncBytes == 0) {
        DBGLN("        no bytes -- check the cable and a common ground");
      } else if (syncGood == 0) {
        DBGLN("        bytes but no valid packets -- do both boards report"
              " the same clk?");
      }
    } else if (syncBad > syncGood / 8) {
      DBGLN("        many corrupt packets -- shorten the wire or drop the"
            " baud rate");
    }
    syncGood = syncBad = syncBytes = 0;
#endif
    frames = 0;
    lastFrameReport = t;
  }
}
