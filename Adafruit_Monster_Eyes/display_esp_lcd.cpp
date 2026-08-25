/**
 * @file display_esp_lcd.cpp
 * @brief Display backend: ESP-IDF esp_lcd, SPI panel IO with async DMA.
 *
 * The Adafruit path pushes pixels with the CPU. esp_lcd hands the buffer to a
 * DMA channel and returns, calling back when it lands, so a frame costs
 * max(render, transfer) rather than their sum.
 *
 * ESP-IDF ships only a few panel drivers in core, ST7789 among them. GC9A01A
 * is not one, so this backend creates an ST7789 panel object -- the addressing
 * commands are identical -- and sends the GC9A01A vendor init itself.
 */

#include "eye.h"

#if EYE_DISPLAY == EYE_DISPLAY_ESP_LCD

#  include <Arduino.h>
#  include <driver/spi_master.h>
#  include <esp_lcd_panel_io.h>
#  include <esp_lcd_panel_vendor.h>
#  include <esp_lcd_panel_ops.h>
#  include <esp_heap_caps.h>

// GC9A01A vendor initialisation, transcribed from Adafruit_GC9A01A.cpp.
// Format: command, length, data... A length with 0x80 set means "then wait".
//
// The GC9A01A shares ST77xx's addressing commands (CASET 0x2A, RASET 0x2B,
// RAMWR 0x2C), so esp_lcd's ST7789 panel object drives it correctly for
// pixels -- only this power-on sequence differs
#  if ESP_LCD_DRIVER == ESP_LCD_DRV_GC9A01A
static const uint8_t gc9a01aInit[] = {
    0xEF, 0,    0xEB, 1,    0x14, 0xFE, 0,    0xEF, 0,    0xEB, 1,    0x14,
    0x84, 1,    0x40, 0x85, 1,    0xFF, 0x86, 1,    0xFF, 0x87, 1,    0xFF,
    0x88, 1,    0x0A, 0x89, 1,    0x21, 0x8A, 1,    0x00, 0x8B, 1,    0x80,
    0x8C, 1,    0x01, 0x8D, 1,    0x01, 0x8E, 1,    0xFF, 0x8F, 1,    0xFF,
    0xB6, 2,    0x00, 0x00, 0x36, 1,    0x48, // MADCTL: MX | BGR
    0x3A, 1,    0x05,                         // COLMOD: 16 bits per pixel
    0x90, 4,    0x08, 0x08, 0x08, 0x08, 0xBD, 1,    0x06, 0xBC, 1,    0x00,
    0xFF, 3,    0x60, 0x01, 0x04, 0xC3, 1,    0x13, // POWER2
    0xC4, 1,    0x13,                               // POWER3
    0xC9, 1,    0x22,                               // POWER4
    0xBE, 1,    0x11, 0xE1, 2,    0x10, 0x0E, 0xDF, 3,    0x21, 0x0C, 0x02,
    0xF0, 6,    0x45, 0x09, 0x08, 0x08, 0x26, 0x2A, // Gamma 1
    0xF1, 6,    0x43, 0x70, 0x72, 0x36, 0x37, 0x6F, // Gamma 2
    0xF2, 6,    0x45, 0x09, 0x08, 0x08, 0x26, 0x2A, // Gamma 3
    0xF3, 6,    0x43, 0x70, 0x72, 0x36, 0x37, 0x6F, // Gamma 4
    0xED, 2,    0x1B, 0x0B, 0xAE, 1,    0x77, 0xCD, 1,    0x63, 0xE8, 1,
    0x34, // Frame rate
    0x62, 12,   0x18, 0x0D, 0x71, 0xED, 0x70, 0x70, 0x18, 0x0F, 0x71, 0xEF,
    0x70, 0x70, 0x63, 12,   0x18, 0x11, 0x71, 0xF1, 0x70, 0x70, 0x18, 0x13,
    0x71, 0xF3, 0x70, 0x70, 0x64, 7,    0x28, 0x29, 0xF1, 0x01, 0xF1, 0x00,
    0x07, 0x66, 10,   0x3C, 0x00, 0xCD, 0x67, 0x45, 0x45, 0x10, 0x00, 0x00,
    0x00, 0x67, 10,   0x00, 0x3C, 0x00, 0x00, 0x00, 0x01, 0x54, 0x10, 0x32,
    0x98, 0x74, 7,    0x10, 0x85, 0x80, 0x00, 0x00, 0x4E, 0x00, 0x98, 2,
    0x3E, 0x07, 0x35, 0, // Tearing effect on
    0x21, 0,             // Inversion on
    0x11, 0x80,          // Sleep out, then wait
    0x29, 0x80,          // Display on, then wait
    0x00                 // End of list
};
#  endif

static esp_lcd_panel_io_handle_t ioHandle[NUM_EYES] = {};
static esp_lcd_panel_handle_t panelHandle[NUM_EYES] = {};

// STRIPE BUFFERING
static uint16_t *scratch = NULL; // 2 stripe buffers, DMA-capable
static uint8_t scratchIdx = 0;
static int stripeW = 1;    // Columns per draw_bitmap call
static int stripeBase = 0; // First column of the stripe being filled
static int panelW = 0, panelH = 0;
static int originX = 0, originY = 0, eyeSize = 0;

static volatile int pending[NUM_EYES] = {};

int displayColumnStride = -1; // Set to -stripeW once the eye size is known
volatile uint32_t displayBusyMicros = 0;

static const int csPin[NUM_EYES] = {TFT_CS
#  if NUM_EYES > 1
                                    ,
                                    TFT1_CS
#  endif
};
static const int dcPin[NUM_EYES] = {TFT_DC
#  if NUM_EYES > 1
                                    ,
                                    TFT1_DC
#  endif
};
static const int rstPin[NUM_EYES] = {TFT_RST
#  if NUM_EYES > 1
                                     ,
                                     TFT1_RST
#  endif
};

static bool IRAM_ATTR onColorDone(esp_lcd_panel_io_handle_t io,
                                  esp_lcd_panel_io_event_data_t *ev,
                                  void *ctx) {
  (void)io;
  (void)ev;
  int e = (int)(intptr_t)ctx;
  if (pending[e] > 0) pending[e]--;
  return false;
}

// Wait until at most `limit` transfers are outstanding for this eye. Bounded,
// so a misconfigured panel cannot lock the sketch up.
static void drain(int e, int limit) {
  uint32_t guard = 0;
  while ((pending[e] > limit) && (++guard < 2000000)) { /* spin */
  }
  if (guard >= 2000000) {
    static bool warned = false;
    if (!warned) {
      warned = true;
      Serial.println("esp_lcd transfer stalled -- try EYE_DISPLAY_TFT.");
    }
    pending[e] = 0;
  }
}

#  if ESP_LCD_DRIVER == ESP_LCD_DRV_GC9A01A
static void sendVendorInit(esp_lcd_panel_io_handle_t io) {
  const uint8_t *p = gc9a01aInit;
  uint8_t cmd;
  while ((cmd = *p++) != 0x00) {
    uint8_t x = *p++;
    uint8_t n = x & 0x7F;
    esp_lcd_panel_io_tx_param(io, cmd, n ? p : NULL, n);
    p += n;
    if (x & 0x80) delay(150);
  }
}
#  endif

bool displayBegin(void) {
  spi_bus_config_t bus = {};
  bus.sclk_io_num = TFT_SCK;
  bus.mosi_io_num = TFT_MOSI;
  bus.miso_io_num = -1;
  bus.quadwp_io_num = -1;
  bus.quadhd_io_num = -1;
  // Largest single transfer: one column of 16-bit pixels.
  bus.max_transfer_sz = TFT_H * 2 + 64;

  if (spi_bus_initialize((spi_host_device_t)ESP_LCD_HOST, &bus,
                         SPI_DMA_CH_AUTO) != ESP_OK) {
    Serial.println("spi_bus_initialize failed");
    return false;
  }

  eyePanelReset(rstPin, NUM_EYES);

  for (int e = 0; e < NUM_EYES; e++) {
    esp_lcd_panel_io_spi_config_t io = {};
    io.cs_gpio_num = csPin[e];
    io.dc_gpio_num = dcPin[e];
    io.spi_mode = 0;
    io.pclk_hz = TFT_SPI_HZ;
    io.trans_queue_depth = 4;
    io.lcd_cmd_bits = 8;
    io.lcd_param_bits = 8;
    io.on_color_trans_done = onColorDone;
    io.user_ctx = (void *)(intptr_t)e;

    if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)ESP_LCD_HOST, &io,
                                 &ioHandle[e]) != ESP_OK) {
      Serial.printf("panel IO %d failed\n", e);
      return false;
    }

    esp_lcd_panel_dev_config_t pc = {};
    // GC9A01A wants BGR ordering; its stock MADCTL is MX | BGR.
    // Reset is driven once
    pc.reset_gpio_num = -1;
    pc.bits_per_pixel = 16;
#  if ESP_LCD_DRIVER == ESP_LCD_DRV_GC9A01A
#    define EYE_RGB_ORDER_BGR 1
#  else
#    define EYE_RGB_ORDER_BGR 0
#  endif
#  if defined(ESP_IDF_VERSION) && defined(ESP_IDF_VERSION_VAL)
#    if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 2, 0)
    pc.rgb_ele_order = EYE_RGB_ORDER_BGR ? LCD_RGB_ELEMENT_ORDER_BGR
                                         : LCD_RGB_ELEMENT_ORDER_RGB;
#    else
    pc.rgb_endian = EYE_RGB_ORDER_BGR ? LCD_RGB_ENDIAN_BGR : LCD_RGB_ENDIAN_RGB;
#    endif
#  else
    pc.rgb_endian = EYE_RGB_ORDER_BGR ? LCD_RGB_ENDIAN_BGR : LCD_RGB_ENDIAN_RGB;
#  endif

    if (esp_lcd_new_panel_st7789(ioHandle[e], &pc, &panelHandle[e]) != ESP_OK) {
      Serial.printf("panel %d failed\n", e);
      return false;
    }
#  if ESP_LCD_DRIVER == ESP_LCD_DRV_GC9A01A
    // The vendor sequence does everything panel_init() would (sleep out,
    // MADCTL, COLMOD, display on)
    sendVendorInit(ioHandle[e]);
#  else
    esp_lcd_panel_init(panelHandle[e]);
#  endif
    esp_lcd_panel_invert_color(panelHandle[e], ESP_LCD_INVERT ? true : false);
    esp_lcd_panel_swap_xy(panelHandle[e], ESP_LCD_SWAP_XY ? true : false);
    esp_lcd_panel_mirror(panelHandle[e], ESP_LCD_MIRROR_X ? true : false,
                         ESP_LCD_MIRROR_Y ? true : false);
    esp_lcd_panel_disp_on_off(panelHandle[e], true);
    pending[e] = 0;
    DBG("  esp_lcd panel %d ready (CS=GPIO%d DC=GPIO%d)\n", e, csPin[e],
        dcPin[e]);
  }

  panelW = TFT_W;
  panelH = TFT_H;
#  if TFT_BACKLIGHT >= 0
  pinMode(TFT_BACKLIGHT, OUTPUT);
  digitalWrite(TFT_BACKLIGHT, HIGH);
#  endif
  DBG("esp_lcd up: %d panel(s), %dx%d at %ld Hz\n", NUM_EYES, panelW, panelH,
      (long)TFT_SPI_HZ);
  return true;
}

int displayMaxEyeSize(void) {
  return (panelW < panelH) ? panelW : panelH;
}

void displaySetEyeSize(int size) {
  eyeSize = size;
  originX = (panelW - size) / 2;
  originY = (panelH - size) / 2;
  if (originX < 0) originX = 0;
  if (originY < 0) originY = 0;

  stripeW = 1;
  for (int w = ESP_LCD_STRIPE_COLS; w >= 1; w--) {
    if ((size % w) == 0) {
      stripeW = w;
      break;
    }
  }
  displayColumnStride = -stripeW;

  if (scratch) heap_caps_free(scratch);
  // MALLOC_CAP_DMA: handed straight to a DMA channel.
  scratch = (uint16_t *)heap_caps_malloc((size_t)stripeW * size * 2 *
                                             sizeof(uint16_t),
                                         MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
  scratchIdx = 0;
  stripeBase = 0;
  if (!scratch) {
    Serial.println("DMA stripe buffer allocation failed!");
    return;
  }
  DBG("  esp_lcd stripes: %d columns, %d draws per eye, %u bytes\n", stripeW,
      size / stripeW,
      (unsigned)((size_t)stripeW * size * 2 * sizeof(uint16_t)));
}

static void fillPanel(int e, uint16_t color) {
  if (!scratch) return;
  const int w = stripeW;
  uint16_t *buf = scratch;
  for (int i = 0; i < w * panelH; i++) buf[i] = color;
  for (int x = 0; x < panelW; x += w) {
    int cols = (x + w <= panelW) ? w : (panelW - x);
    if (cols != w) break; // Leave a ragged edge rather than corrupt memory
    drain(e, 0);
    esp_lcd_panel_draw_bitmap(panelHandle[e], x, 0, x + cols, panelH, buf);
    pending[e]++;
  }
  drain(e, 0);
}

void displayClear(uint16_t color) {
  for (int e = 0; e < NUM_EYES; e++) fillPanel(e, OUT16(color));
}

void displayFrameBegin(void) {}
void displayEyeBegin(int eye) {
  (void)eye;
}

uint16_t *displayColumn(int eye, int x) {
  (void)eye;
  if (!scratch) return NULL;
  const int c = x % stripeW; // Column within the stripe
  uint16_t *base = &scratch[(size_t)scratchIdx * stripeW * eyeSize];
  if (c == 0) stripeBase = x;
  // Bottom row of this column; the renderer walks upward by one stripe width.
  return &base[(size_t)(eyeSize - 1) * stripeW + c];
}

void displayColumnDone(int eye, int x) {
  if (!scratch) return;
#  if PROFILE_FRAME
  uint32_t t0 = micros();
#  endif
  // Only send once the stripe is full.
  if (((x % stripeW) == (stripeW - 1)) || (x == eyeSize - 1)) {
    esp_lcd_panel_draw_bitmap(panelHandle[eye], originX + stripeBase, originY,
                              originX + stripeBase + stripeW, originY + eyeSize,
                              &scratch[(size_t)scratchIdx * stripeW * eyeSize]);
    pending[eye]++;
    scratchIdx ^= 1;
    drain(eye, 1);
  }
#  if PROFILE_FRAME
  displayBusyMicros += micros() - t0;
#  endif
}

void displayEyeEnd(int eye) {
  drain(eye, 0);
}
void displayFrameEnd(void) {}

void displaySelfTest(void) {
  const uint16_t idColor[2] = {0xF800, 0x001F};
  const char *idName[2] = {"RED", "BLUE"};
  for (int e = 0; e < NUM_EYES; e++) {
    DBG("  self-test: panel %d -> %s   (CS=GPIO%d DC=GPIO%d)\n", e, idName[e],
        csPin[e], dcPin[e]);
    fillPanel(e, OUT16(idColor[e]));
  }
  DBGLN("  if a panel stays dark, its CS/DC GPIO numbers are wrong");
  delay(2500);
  for (int e = 0; e < NUM_EYES; e++) fillPanel(e, 0);
}

#endif // EYE_DISPLAY == EYE_DISPLAY_ESP_LCD
