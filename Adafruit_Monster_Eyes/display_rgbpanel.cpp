/**
 * @file display_rgbpanel.cpp
 * @brief Display backend: parallel RGB666 panels via Arduino_GFX (Qualia S3).
 *
 *     eye 240   126 KB   internal SRAM
 *     eye 480   484 KB   PSRAM only
 *     eye 720  1076 KB   PSRAM only
 *
 * Inert unless EYE_DISPLAY selects it, so no other build needs Arduino_GFX.
 */

#include "eye.h"

#if EYE_DISPLAY == EYE_DISPLAY_RGB

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <Wire.h>
#include <esp_heap_caps.h>

// ---------------------------------------------------------------------------
// Panel geometry and timing. Only the round displays
// ---------------------------------------------------------------------------
#if QUALIA_PANEL == QUALIA_PANEL_21_480
#define RGB_W 480 ///< Panel width in pixels
#define RGB_H 480 ///< Panel height in pixels
#define RGB_INIT_OPS TL021WVC02_init_operations
#define RGB_HS_POL 1 ///< HSYNC polarity
#define RGB_HS_FP 50 ///< HSYNC front porch
#define RGB_HS_PW 2  ///< HSYNC pulse width
#define RGB_HS_BP 44 ///< HSYNC back porch
#define RGB_VS_POL 1 ///< VSYNC polarity
#define RGB_VS_FP 16 ///< VSYNC front porch
#define RGB_VS_PW 2  ///< VSYNC pulse width
#define RGB_VS_BP 18 ///< VSYNC back porch

#elif QUALIA_PANEL == QUALIA_PANEL_28_480
#define RGB_W 480 ///< Panel width in pixels
#define RGB_H 480 ///< Panel height in pixels
#define RGB_INIT_OPS TL028WVC01_init_operations
#define RGB_HS_POL 1 ///< HSYNC polarity
#define RGB_HS_FP 50 ///< HSYNC front porch
#define RGB_HS_PW 2  ///< HSYNC pulse width
#define RGB_HS_BP 44 ///< HSYNC back porch
#define RGB_VS_POL 1 ///< VSYNC polarity
#define RGB_VS_FP 16 ///< VSYNC front porch
#define RGB_VS_PW 2  ///< VSYNC pulse width
#define RGB_VS_BP 18 ///< VSYNC back porch

#elif QUALIA_PANEL == QUALIA_PANEL_40_720
// The 720 round needs its own porch values; the 480 numbers do not drive it.
#define RGB_W 720 ///< Panel width in pixels
#define RGB_H 720 ///< Panel height in pixels
#define RGB_INIT_OPS hd40015c40_init_operations
#define RGB_HS_POL 1 ///< HSYNC polarity
#define RGB_HS_FP 46 ///< HSYNC front porch
#define RGB_HS_PW 2  ///< HSYNC pulse width
#define RGB_HS_BP 44 ///< HSYNC back porch
#define RGB_VS_POL 1 ///< VSYNC polarity
#define RGB_VS_FP 50 ///< VSYNC front porch
#define RGB_VS_PW 16 ///< VSYNC pulse width
#define RGB_VS_BP 16 ///< VSYNC back porch

#else
#error "Unknown QUALIA_PANEL -- see settings.h for the round displays"
#endif

/// Expander driving reset, chip select and the init-sequence clock over I2C.
static Arduino_XCA9554SWSPI *expander = NULL;
/// The parallel RGB timing generator.
static Arduino_ESP32RGBPanel *rgbpanel = NULL;
/// GFX object owning the PSRAM framebuffer.
static Arduino_RGB_Display *gfx = NULL;

static uint16_t *stripe = NULL;      ///< Rendered stripe, EYE_SCALE times small
static uint16_t *blitbuf = NULL;     ///< Same stripe expanded to panel pixels
static int stripeW = 1;              ///< Rendered columns per blit
static int stripeBase = 0;           ///< First rendered column of the stripe
static int eyeSize = 0;              ///< Rendered eye size, before scaling
static int originX = 0, originY = 0; ///< Top-left of the eye, panel pixels

int displayColumnStride = -1;            ///< Set to -stripeW once size is known
volatile uint32_t displayBusyMicros = 0; ///< Time spent blitting

bool displayBegin(void) {
  Wire.setClock(1000000); // The init sequence is slow at the default rate
  expander = new Arduino_XCA9554SWSPI(PCA_TFT_RESET, PCA_TFT_CS, PCA_TFT_SCK,
                                      PCA_TFT_MOSI, &Wire, 0x3F);
  rgbpanel = new Arduino_ESP32RGBPanel(
      TFT_DE, TFT_VSYNC, TFT_HSYNC, TFT_PCLK, TFT_R1, TFT_R2, TFT_R3, TFT_R4,
      TFT_R5, TFT_G0, TFT_G1, TFT_G2, TFT_G3, TFT_G4, TFT_G5, TFT_B1, TFT_B2,
      TFT_B3, TFT_B4, TFT_B5, RGB_HS_POL, RGB_HS_FP, RGB_HS_PW, RGB_HS_BP,
      RGB_VS_POL, RGB_VS_FP, RGB_VS_PW, RGB_VS_BP);
  gfx = new Arduino_RGB_Display(
      RGB_W, RGB_H, rgbpanel, 0 /* rotation */, true /* auto_flush */, expander,
      GFX_NOT_DEFINED /* RST */, RGB_INIT_OPS, sizeof(RGB_INIT_OPS));

  if (!gfx->begin()) {
    Serial.println("Arduino_GFX begin() failed -- is PSRAM enabled?");
    return false;
  }
  gfx->fillScreen(0);
  expander->pinMode(PCA_TFT_BACKLIGHT, OUTPUT);
  expander->digitalWrite(PCA_TFT_BACKLIGHT, HIGH);

  DBG("RGB666 up: %dx%d, framebuffer %u bytes in PSRAM\n", RGB_W, RGB_H,
      (unsigned)(RGB_W * RGB_H * 2));
  return true;
}

int displayMaxEyeSize(void) {
  int w = (RGB_W < RGB_H) ? RGB_W : RGB_H;
  return w / EYE_SCALE;
}

void displaySetEyeSize(int size) {
  eyeSize = size;
  const int shown = size * EYE_SCALE;
  originX = (RGB_W - shown) / 2;
  originY = (RGB_H - shown) / 2;
  if (originX < 0)
    originX = 0;
  if (originY < 0)
    originY = 0;

  stripeW = 1;
  for (int w = RGB_STRIPE_COLS; w >= 1; w--) {
    if ((size % w) == 0) {
      stripeW = w;
      break;
    }
  }
  displayColumnStride = -stripeW;

  free(stripe);
  heap_caps_free(blitbuf);
  stripe = (uint16_t *)eyeMalloc((size_t)stripeW * size * sizeof(uint16_t));
  blitbuf = (uint16_t *)heap_caps_malloc((size_t)stripeW * EYE_SCALE * size *
                                             EYE_SCALE * sizeof(uint16_t),
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!blitbuf) {
    blitbuf = (uint16_t *)malloc((size_t)stripeW * EYE_SCALE * size *
                                 EYE_SCALE * sizeof(uint16_t));
  }
  stripeBase = 0;
  if (!stripe || !blitbuf) {
    Serial.println("Stripe buffer allocation failed!");
    return;
  }
  DBG("  rendering %dx%d, showing %dx%d (scale %d), stripe %d cols\n", size,
      size, shown, shown, EYE_SCALE, stripeW);
}

void displayClear(uint16_t color) {
  if (gfx)
    gfx->fillScreen(color);
}

void displayFrameBegin(void) {}
void displayEyeBegin(int eye) { (void)eye; }

uint16_t *displayColumn(int eye, int x) {
  (void)eye;
  if (!stripe)
    return NULL;
  const int c = x % stripeW;
  if (c == 0)
    stripeBase = x;
  return &stripe[(size_t)(eyeSize - 1) * stripeW + c];
}

void displayColumnDone(int eye, int x) {
  (void)eye;
  if (!stripe || !blitbuf)
    return;
  if (((x % stripeW) != (stripeW - 1)) && (x != eyeSize - 1))
    return;

#if PROFILE_FRAME
  uint32_t t0 = micros();
#endif
  const int S = EYE_SCALE;
  const int outW = stripeW * S;

  for (int r = 0; r < eyeSize; r++) {
    const uint16_t *src = &stripe[(size_t)r * stripeW];
    for (int sy = 0; sy < S; sy++) {
      uint16_t *dst = &blitbuf[(size_t)(r * S + sy) * outW];
      for (int c = 0; c < stripeW; c++) {
        const uint16_t v = src[c];
        for (int sx = 0; sx < S; sx++)
          *dst++ = v;
      }
    }
  }
  gfx->draw16bitRGBBitmap(originX + stripeBase * S, originY, blitbuf, outW,
                          eyeSize * S);
#if PROFILE_FRAME
  displayBusyMicros += micros() - t0;
#endif
}

void displayEyeEnd(int eye) { (void)eye; }
void displayFrameEnd(void) {}

void displaySelfTest(void) {
  const uint16_t bars[4] = {0xF800, 0x07E0, 0x001F, 0xFFFF};
  for (uint8_t i = 0; i < 4; i++) {
    gfx->fillScreen(bars[i]);
    delay(600);
  }
  gfx->fillScreen(0);
  gfx->fillRect(0, 0, RGB_W / 2, RGB_H / 2, 0xFFE0);
  DBGLN("  yellow block should sit TOP-LEFT");
  delay(1200);
}

#endif // EYE_DISPLAY == EYE_DISPLAY_RGB
