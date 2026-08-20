// M4_Eyes ported to PicoDVI (RP2040 / RP2350).
//
// Original: Phillip Burgess for Adafruit Industries, MIT license.
// This port keeps the eye math intact and replaces the SAMD51 SPI-DMA output
// path with a framebuffer. Everything that existed only to feed two ST7789s
// over DMA is gone: columnStruct, renderBuf, DmacDescriptor lists, DMAbuddy,
// dma_busy / column_ready, the DMA stall timeout, and the eyelidIndex trick
// (which required both bytes of the eyelid color to be identical). Two eyes
// now cost barely more RAM than one, because the polar maps, displacement
// table, textures and eyelid tables are all shared. This build renders ONE
// eye, centered.
//
// All assets live on the USB drive; nothing is compiled in. Hold BOOTSEL at
// reset to expose the drive and drop in a different eye.
//
// COORDINATE SYSTEM. The original rendered into a display set to rotation 3,
// which put the code in a frame where +Y is UP and +X is RIGHT, with the eye
// drawn column at a time. That frame is preserved here, so writing to the
// framebuffer is just a vertical flip:
//
//     fb[(ORIGIN_Y + SIZE - 1 - y) * FB_WIDTH + ORIGIN_X + x]
//
// No rotation, no transpose. Confirmed against the eyelid tables, where the
// upper lid occupies high y and image row 0 maps to y = SIZE-1.
//
// TEARING. DVIGFX16 cannot double-buffer -- there is not enough RAM. Since
// rendering runs column by column across every scanline, fast saccades can
// shear. If that bothers you, DVIGFX8 has a real swap() synced to vsync; see
// the notes at the bottom of this file.

#include <PicoDVI.h>
#include <math.h>
#include "eye.h"

// Live geometry, taken from `settings` once at startup so the render loop
// never dereferences the settings struct per pixel.
static int SIZE, HALF, ORIGIN_X, ORIGIN_Y;

// Pin config comes from DVI_PIN_CONFIG in eye.h -- set it for your board.
DVIGFX16 display(DVI_RES_320x240p60, DVI_PIN_CONFIG);

// EYE STATE ---------------------------------------------------------------

#define NOBLINK 0
#define ENBLINK 1
#define DEBLINK 2

typedef struct {
  float    irisSpin;       // RPM * -1024 (negative is clockwise to a viewer)
  uint16_t irisStartAngle; // 0-1023 CCW
  uint16_t irisAngle;

  uint8_t  blinkState;
  uint32_t blinkDuration;
  uint32_t blinkStartTime;
  float    blinkFactor;

  float    eyeX, eyeY;     // Position in map space, saved per eye to avoid tearing
  float    pupilFactor;
  float    upperLidFactor, lowerLidFactor;
} eyeState;

static eyeState eye[NUM_EYES];

// Shared animation state
static bool     eyeInMotion      = false;
static float    eyeOldX, eyeOldY, eyeNewX, eyeNewY;
static uint32_t eyeMoveStartTime = 0;
static int32_t  eyeMoveDuration  = 0;
static uint32_t lastSaccadeStop  = 0;
static int32_t  saccadeInterval  = 0;
static uint32_t timeOfLastBlink  = 0;
static uint32_t timeToNextBlink  = 0;
static float    frameEyeX, frameEyeY;

// Autonomous iris scaling via fractal subdivision (no light sensor here)
#define IRIS_LEVELS 7
static float    irisPrev[IRIS_LEVELS] = {0};
static float    irisNext[IRIS_LEVELS] = {0};
static uint16_t irisFrame = 0;
static float    irisValue = 0.5f;
static float    irisMin, irisRange;

static uint32_t frames = 0;
static uint32_t lastFrameReport = 0;

// SETUP -------------------------------------------------------------------

void setup() {
  Serial.begin(115200);

  // Drive mode is checked FIRST, before DVI touches core1 or the PIOs.
  // See eye_storage.cpp for why the two modes cannot run at the same time.
  if (eyeStorageDriveModeRequested()) {
    eyeStorageRunDriveMode();   // Never returns; reboots on eject
  }

  eyeSettingsDefaults();
  if (eyeStorageBegin()) {
    eyeSettingsLoad(CONFIG_FILENAME);
  }
  eyeSettingsFinalize();

  // Framebuffer first: it is the single largest allocation and must not have
  // to fight fragmentation from anything else.
  if (!display.begin()) {
    // Framebuffer allocation failed. Almost always means the eye tables or
    // something else claimed RAM first, or the resolution is too large.
    pinMode(LED_BUILTIN, OUTPUT);
    for (;;) digitalWrite(LED_BUILTIN, (millis() / 200) & 1);
  }
  display.fillScreen(0);
  Serial.printf("Framebuffer up. Free heap: %u\n", rp2040.getFreeHeap());

  // pupilMin/pupilMax in the file are the inverse of the irisMin/irisRange
  // the renderer wants.
  irisMin   = 1.0f - settings.pupilMax;
  irisRange = settings.pupilMax - settings.pupilMin;

  // Build the polar and displacement maps. If a config asks for an eye too
  // large for the remaining heap, step the size down rather than failing --
  // the maps grow as mapRadius^2, so this converges quickly.
  uint32_t t0 = millis();
  while (!eyeTablesInit()) {
    if (settings.displaySize <= 96) {
      Serial.println("Cannot allocate eye tables even at minimum size.");
      for (;;) delay(1000);
    }
    settings.displaySize -= 16;
    settings.eyeRadius    = settings.displaySize / 2 + 5;
    eyeSettingsFinalize();
    Serial.printf("Not enough RAM; retrying at displaySize %d\n", settings.displaySize);
  }
  Serial.printf("Tables built in %lu ms (size %d, mapRadius %d). Free heap: %u\n",
                millis() - t0, settings.displaySize, mapRadius, rp2040.getFreeHeap());

  // Whatever is left, minus a reserve, is the texture budget.
  uint32_t freeHeap = rp2040.getFreeHeap();
  uint32_t texBudget = (freeHeap > HEAP_RESERVE) ? (freeHeap - HEAP_RESERVE) : 0;
  if (!eyeMediaLoad(settings.displaySize, texBudget)) {
    Serial.println("Eyelid table allocation failed.");
    for (;;) delay(1000);
  }

  // Nothing else reads the filesystem; let go of it so flash stays quiet.
  eyeStorageEnd();

  SIZE     = settings.displaySize;
  HALF     = SIZE / 2;
  ORIGIN_X = (FB_WIDTH  - SIZE) / 2;
  ORIGIN_Y = (FB_HEIGHT - SIZE) / 2;
  Serial.printf("Running. Free heap: %u\n", rp2040.getFreeHeap());

  for (uint8_t e = 0; e < NUM_EYES; e++) {
    eye[e].irisSpin       = -1024.0f * settings.irisSpin;
    eye[e].irisStartAngle = settings.irisStartAngle;
    eye[e].irisAngle      = eye[e].irisStartAngle;
    eye[e].blinkState     = NOBLINK;
    eye[e].blinkFactor    = 0.0f;
    eye[e].pupilFactor    = 0.5f;
    eye[e].upperLidFactor = 1.0f;
    eye[e].lowerLidFactor = 1.0f;
    eye[e].eyeX = eye[e].eyeY = (float)mapRadius;
  }

  eyeOldX = eyeNewX = eyeOldY = eyeNewY = (float)mapRadius;
  frameEyeX = frameEyeY = (float)mapRadius;

  randomSeed(micros());
}

// ONCE-PER-FRAME ANIMATION ------------------------------------------------

static void updateGaze(uint32_t t) {
  int32_t dt = t - eyeMoveStartTime;

  if (eyeInMotion) {
    if (dt >= eyeMoveDuration) {          // Destination reached
      eyeInMotion = false;
      uint32_t limit = min((uint32_t)1000000, settings.gazeMax);
      eyeMoveDuration = random(35000, limit);  // Hold before next microsaccade
      if (!saccadeInterval) {
        lastSaccadeStop = t;
        saccadeInterval = random(eyeMoveDuration, settings.gazeMax);
      }
      eyeMoveStartTime = t;
      frameEyeX = eyeOldX = eyeNewX;
      frameEyeY = eyeOldY = eyeNewY;
    } else {                              // Interpolate, ease in/out
      float e = (float)dt / (float)eyeMoveDuration;
      e = 3.0f * e * e - 2.0f * e * e * e;
      frameEyeX = eyeOldX + (eyeNewX - eyeOldX) * e;
      frameEyeY = eyeOldY + (eyeNewY - eyeOldY) * e;
    }
  } else {
    frameEyeX = eyeOldX;
    frameEyeY = eyeOldY;
    if (dt > eyeMoveDuration) {
      if ((t - lastSaccadeStop) > (uint32_t)saccadeInterval) {
        // Full saccade. r is how far the gaze can travel from center; it is
        // what collapses if COVERAGE is set too low.
        float r = ((float)mapDiameter - (float)SIZE * (float)M_PI_2) * 0.75f;
        eyeNewX = random(-r, r);
        float h = sqrtf(r * r - eyeNewX * eyeNewX);
        eyeNewY = random(-h, h);
        eyeMoveDuration = random(83000, 166000);
        saccadeInterval = 0;
      } else {
        // Microsaccade, roughly 1/10 the radius. No clipping: a slight stray
        // is corrected by the next full saccade.
        float r = ((float)mapDiameter - (float)SIZE * (float)M_PI_2) * 0.07f;
        float dx = random(-r, r);
        eyeNewX = frameEyeX - mapRadius + dx;
        float h = sqrtf(r * r - dx * dx);
        eyeNewY = frameEyeY - mapRadius + random(-h, h);
        eyeMoveDuration = random(7000, 25000);
      }
      eyeNewX += mapRadius;   // Into map space
      eyeNewY += mapRadius;
      eyeMoveStartTime = t;
      eyeInMotion = true;
    }
  }
}

static void updateIris(void) {
  float n, sum = 0.5f;
  for (uint16_t i = 0; i < IRIS_LEVELS; i++) {
    uint16_t iexp  = 1 << (i + 1);
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
  if ((++irisFrame) >= (1 << IRIS_LEVELS)) irisFrame = 0;
}

static void updateBlinks(uint32_t t) {
  if ((t - timeOfLastBlink) >= timeToNextBlink) {
    timeOfLastBlink = t;
    uint32_t d = random(36000, 72000);
    for (uint8_t e = 0; e < NUM_EYES; e++) {
      if (eye[e].blinkState == NOBLINK) {
        eye[e].blinkState     = ENBLINK;
        eye[e].blinkStartTime = t;
        eye[e].blinkDuration  = d;
      }
    }
    timeToNextBlink = d * 3 + random(4000000);
  }
}

static void updateEye(uint8_t e, uint32_t t) {
  eyeState &E = eye[e];

  // In the two-eye build the eyes converged slightly toward the center of
  // the face; a single centered eye has nothing to converge toward.
  E.eyeX = frameEyeX;   // No second eye to converge toward
  E.eyeY = frameEyeY;
  E.pupilFactor = irisValue;

  float uq, lq;
  if (settings.tracking) {
    int ix = (int)map2screen((float)mapRadius - E.eyeX) + HALF;
    int iy = (int)map2screen((float)mapRadius - E.eyeY) + HALF;
    iy += (int)(settings.irisRadius * settings.trackFactor);
    if (settings.eyelidMirror) ix = SIZE - 1 - ix;
    if (ix < 0) ix = 0; else if (ix > SIZE - 1) ix = SIZE - 1;
    if (iy > upperOpen[ix])        uq = 1.0f;
    else if (iy < upperClosed[ix]) uq = 0.0f;
    else uq = (float)(iy - upperClosed[ix]) /
              (float)(upperOpen[ix] - upperClosed[ix]);
    lq = 1.0f - uq;
  } else {
    uq = lq = 1.0f;   // Fully open when not blinking
  }
  E.upperLidFactor = (E.upperLidFactor * 0.6f) + (uq * 0.4f);
  E.lowerLidFactor = (E.lowerLidFactor * 0.6f) + (lq * 0.4f);

  if (E.blinkState) {
    if ((t - E.blinkStartTime) >= E.blinkDuration) {
      if (++E.blinkState > DEBLINK) {
        E.blinkState  = NOBLINK;
        E.blinkFactor = 0.0f;
      } else {
        E.blinkDuration *= 2;   // Opening is half the speed of closing
        E.blinkStartTime = t;
        E.blinkFactor = 1.0f;
      }
    } else {
      E.blinkFactor = (float)(t - E.blinkStartTime) / (float)E.blinkDuration;
      if (E.blinkState == DEBLINK) E.blinkFactor = 1.0f - E.blinkFactor;
    }
  }

  // Cast through int32_t, NOT straight to uint16_t. Once irisSpin * mins goes
  // negative, a direct float->unsigned conversion is undefined behavior, and
  // ARM's __aeabi_f2uiz saturates it to 0 -- which pins the iris angle at zero
  // and the iris stops spinning. Going via a signed int wraps correctly.
  float mins = (float)millis() / 60000.0f;
  E.irisAngle = (uint16_t)(int32_t)((float)E.irisStartAngle +
                                    E.irisSpin * mins + 0.5f);
}

// RENDER ------------------------------------------------------------------

static void renderEye(uint8_t e) {
  eyeState &E  = eye[e];
  uint16_t *fb = display.getBuffer();
  const int half = HALF;

  const int xPositionOverMap = (int)(E.eyeX - (float)half);
  const int yPositionOverMap = (int)(E.eyeY - (float)half);

  const float upperLidFactor = (1.0f - E.blinkFactor) * E.upperLidFactor;
  const float lowerLidFactor = (1.0f - E.blinkFactor) * E.lowerLidFactor;
  const int   irisH = irisHeight(), irisW = irisWidth();
  const int   scleraH = scleraHeight(), scleraW = scleraWidth();
  const uint16_t *iris = irisData, *sclera = scleraData;
  const int   iPupilFactor =
      (int)((float)irisH * 256.0f * (1.0f / E.pupilFactor));
  const uint16_t irisAngle = E.irisAngle;
  const uint16_t pupilColor = settings.pupilColor;
  const uint16_t backColor = settings.backColor;
  const uint16_t eyelidColor = settings.eyelidColor;
  const uint16_t irisMirror = settings.irisMirror;
  const uint16_t scleraMirror = settings.scleraMirror;
  const uint16_t scleraAngle = settings.scleraStartAngle;
  const bool  mirrorLids = settings.eyelidMirror;

  for (int x = 0; x < SIZE; x++) {
    const int lidColumn = mirrorLids ? (SIZE - 1 - x) : x;

    // Destination pointer starts at the TOP of the column and walks up-screen
    // (i.e. backward through memory) as y increases.
    uint16_t *dst = &fb[(ORIGIN_Y + SIZE - 1) * FB_WIDTH + ORIGIN_X + x];

    int y1 = (int)lowerClosed[lidColumn] +
             (int)(0.5f + lowerLidFactor * (float)((int)lowerOpen[lidColumn] -
                                                   (int)lowerClosed[lidColumn]));
    int y2 = (int)upperClosed[lidColumn] +
             (int)(0.5f + upperLidFactor * (float)((int)upperOpen[lidColumn] -
                                                   (int)upperClosed[lidColumn]));
    if (y1 > SIZE - 1) y1 = SIZE - 1; else if (y1 < 0) y1 = 0;
    if (y2 > SIZE - 1) y2 = SIZE - 1; else if (y2 < 0) y2 = 0;

    if (y1 >= y2) {
      // Lid closed far enough that no eye pixels show in this column
      for (int y = 0; y < SIZE; y++, dst -= FB_WIDTH) *dst = eyelidColor;
      continue;
    }

    // Lower eyelid
    int y = 0;
    for (; y < y1; y++, dst -= FB_WIDTH) *dst = eyelidColor;

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

    for (; y <= y2; y++, dst -= FB_WIDTH) {
      const int yy = yPositionOverMap + y;
      int doff, dx, dy;

      if (y < half) {
        doff = (half - 1) - y;
        dy   = -(int)displaceY[doff];
      } else {
        doff = y - half;
        dy   =  (int)displaceY[doff];
      }
      dx = displaceX[doff * half];

      if (dx >= 255) {                 // Outside the eyeball
        *dst = eyelidColor;
        continue;
      }
      dx *= xmul;
      int mx = xx + dx;
      int my = yy + dy;

      if ((mx < 0) || (mx >= mapDiameter) || (my < 0) || (my >= mapDiameter)) {
        *dst = backColor;              // Off the map
        continue;
      }

      int angle, dist, moff;
      if (my >= mapRadius) {
        if (mx >= mapRadius) {                    // Quadrant 1: direct
          mx   -= mapRadius;
          my   -= mapRadius;
          moff  = my * mapRadius + mx;
          angle = polarAngle[moff];
          dist  = polarDist[moff];
        } else {                                  // Quadrant 2: rotate 90, mirror X
          mx    = mapRadius - 1 - mx;
          my   -= mapRadius;
          angle = polarAngle[mx * mapRadius + my] + 768;
          dist  = polarDist[my * mapRadius + mx];
        }
      } else {
        if (mx < mapRadius) {                     // Quadrant 3: rotate 180
          mx    = mapRadius - 1 - mx;
          my    = mapRadius - 1 - my;
          moff  = my * mapRadius + mx;
          angle = polarAngle[moff] + 512;
          dist  = polarDist[moff];
        } else {                                  // Quadrant 4: rotate 270, mirror Y
          mx   -= mapRadius;
          my    = mapRadius - 1 - my;
          angle = polarAngle[mx * mapRadius + my] + 256;
          dist  = polarDist[my * mapRadius + mx];
        }
      }

      if (dist >= 0) {                            // Sclera
        int a  = ((angle + scleraAngle) & 1023) ^ scleraMirror;
        int tx = a * scleraW / 1024;
        int ty = dist * scleraH / 128;
        *dst = sclera[ty * scleraW + tx];
      } else if (dist > -128) {                   // Iris or pupil
        int ty = dist * iPupilFactor / -32768;
        if (ty >= irisH) {
          *dst = pupilColor;
        } else {
          int a  = ((angle + irisAngle) & 1023) ^ irisMirror;
          int tx = a * irisW / 1024;
          *dst = iris[ty * irisW + tx];
        }
      } else {
        *dst = backColor;                         // Back of eye
      }
    }

    // Upper eyelid
    for (; y < SIZE; y++, dst -= FB_WIDTH) *dst = eyelidColor;
  }
}

// LOOP --------------------------------------------------------------------

void loop() {
  uint32_t t = micros();

  updateGaze(t);
  updateBlinks(t);
  updateIris();

  for (uint8_t e = 0; e < NUM_EYES; e++) {
    updateEye(e, t);
    renderEye(e);
  }

  frames++;
  if ((t - lastFrameReport) >= 1000000) {
    Serial.printf("%lu fps\n", frames);
    frames = 0;
    lastFrameReport = t;
  }
}

// NOTES -------------------------------------------------------------------
//
// Moving to DVIGFX8 (256-color, 76,800 byte framebuffer, real vsync-synced
// swap()) frees ~77 KB and eliminates tearing. Changes needed:
//   - declare DVIGFX8 display(DVI_RES_320x240p60, true, cfg) for double buffer
//   - getBuffer() returns uint8_t*; textures become palette indices
//   - bake_assets.py must quantize iris + sclera to a shared 256-color palette
//     and emit uint8_t arrays plus the palette itself
//   - call display.setColor() for each palette entry in setup(), then
//     display.swap(false, true) once so both buffers share the palette
//   - call display.swap() at the end of loop()
// The renderer gets slightly faster too, since it writes bytes.
//
// Eye size is now runtime-driven. A config.eye can set "displaySize", and if
// the request does not fit the heap, setup() steps it down 16 at a time until
// it does. The size/RAM table in eye_config.h shows where the limits fall.
//
// On RP2350 the whole question is moot: 520 KB is enough for a 16-bit
// double-buffered 320x240 plus a full-size 240x240 eye and in-RAM textures.
// Adafruit_DVI_HSTX drives DVI from the HSTX peripheral there, freeing a PIO
// and a good deal of core1 time versus PicoDVI's TMDS encoding.
