/**
 * @file display_tft.cpp
 * @brief Display backend: SPI TFT panels driven by Adafruit_GFX.
 *
 * Portable across RP2 and ESP32. On RP2 it additionally batches pixels
 * straight into the SPI hardware and drives them with DMA, so transfers
 * overlap the next column's render.
 *
 * Inert unless @ref EYE_DISPLAY selects it, so other builds need no TFT
 * driver library.
 *
 * COLUMN ORDER. The renderer's +Y is up, so it produces a column bottom-first
 * while the panel wants it top-first. Rather than reverse it in a second pass,
 * displayColumn() hands back a pointer near the END of the stripe buffer with
 * a negative stride; the renderer then fills it in exactly the order the panel
 * wants. This is verified to be pixel-identical to the framebuffer path.
 */

#include "eye.h"

#if EYE_DISPLAY == EYE_DISPLAY_TFT

#  include <Arduino.h>
#  include <SPI.h>
#  if TFT_FAST_SPI_ACTIVE
#    include <hardware/spi.h>
#    if TFT_DMA
#      include <hardware/dma.h>
#    endif
#  endif

#  if TFT_DRIVER == TFT_DRIVER_ST7789
#    include <Adafruit_ST7789.h>
#    define TFT_CLASS Adafruit_ST7789
static Adafruit_ST7789 tft0(&TFT_SPI_PORT, TFT_CS, TFT_DC, -1);
#    if NUM_EYES > 1
static Adafruit_ST7789 tft1(&TFT1_SPI_PORT, TFT1_CS, TFT1_DC, -1);
#    endif
#  elif TFT_DRIVER == TFT_DRIVER_ILI9341
#    include <Adafruit_ILI9341.h>
#    define TFT_CLASS Adafruit_ILI9341
static Adafruit_ILI9341 tft0(&TFT_SPI_PORT, TFT_DC, TFT_CS, -1);
#    if NUM_EYES > 1
static Adafruit_ILI9341 tft1(&TFT1_SPI_PORT, TFT1_DC, TFT1_CS, -1);
#    endif
#  elif TFT_DRIVER == TFT_DRIVER_GC9A01A
#    include <Adafruit_GC9A01A.h>
#    define TFT_CLASS Adafruit_GC9A01A
static Adafruit_GC9A01A tft0(&TFT_SPI_PORT, TFT_DC, TFT_CS, -1);
#    if NUM_EYES > 1
static Adafruit_GC9A01A tft1(&TFT1_SPI_PORT, TFT1_DC, TFT1_CS, -1);
#    endif
#  else
#    error "Unknown TFT_DRIVER"
#  endif

#  if TFT_FAST_SPI_ACTIVE
static spi_inst_t *spiInst[NUM_EYES];
static spi_inst_t *spiForPin(int sck) {
  return (((sck / 8) % 2) == 0) ? spi0 : spi1;
}

#    if !TFT_DMA
static void pushPixels(spi_inst_t *spi, const uint16_t *buf, size_t n) {
  while (spi_is_busy(spi)) tight_loop_contents(); // let commands finish
  spi_set_format(spi, 16, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);

  size_t tx = n, rx = n;
  uint32_t guard = 0;
  const uint32_t guardMax = (uint32_t)n * 64 + 10000;
  while ((tx || rx) && (++guard < guardMax)) {
    if (tx && spi_is_writable(spi)) {
      spi_get_hw(spi)->dr = *buf++;
      tx--;
    }
    if (rx && spi_is_readable(spi)) {
      (void)spi_get_hw(spi)->dr;
      rx--;
    }
  }
  (void)guardMax;
  if (guard >= guardMax) {
    static bool warned = false;
    if (!warned) {
      warned = true;
      Serial.println(
          "SPI burst stalled -- wrong SPI block? Set TFT_FAST_SPI 0.");
    }
  }
  while (spi_is_busy(spi)) tight_loop_contents();
  spi_set_format(spi, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
}
#    endif // !TFT_DMA
#  endif

static TFT_CLASS *panel[NUM_EYES] = {&tft0
#  if NUM_EYES > 1
                                     ,
                                     &tft1
#  endif
};

// Two column buffers per eye when DMA is on: the renderer fills one while the
// other is still being sent. Without DMA a single buffer is enough.
#  if TFT_FAST_SPI_ACTIVE && TFT_DMA
#    define COLUMN_BUFFERS 2
#  else
#    define COLUMN_BUFFERS 1
#  endif
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

// STRIPE BUFFERING
static uint16_t *scratch = NULL; // COLUMN_BUFFERS * stripeW * eyeSize
static uint8_t scratchIdx = 0;
static int stripeW = 1;    // Columns per address window
static int stripeBase = 0; // First column of the stripe in hand

#  if TFT_FAST_SPI_ACTIVE && TFT_DMA
static int dmaCh[NUM_EYES];
static bool dmaActive[NUM_EYES];

// Wait for this eye's transfer to land
static void dmaSettle(int eye) {
  if (!dmaActive[eye]) return;
  uint32_t guard = 0;
  while (dma_channel_is_busy(dmaCh[eye]) && (++guard < 5000000))
    tight_loop_contents();
  if (guard >= 5000000) {
    static bool warned = false;
    if (!warned) {
      warned = true;
      Serial.println("DMA stalled -- set TFT_DMA 0 in eye.h.");
    }
    dma_channel_abort(dmaCh[eye]);
  }
  dmaActive[eye] = false;
  // The channel is done feeding the FIFO; the shifter may still be draining.
  while (spi_is_busy(spiInst[eye])) tight_loop_contents();
}
#  endif
static int panelW = 0, panelH = 0;
static int originX = 0, originY = 0, eyeSize = 0;

int displayColumnStride = -1; // Renderer walks backward through scratch
volatile uint32_t displayBusyMicros = 0;

// Reset every panel ONCE, before any of them is initialized.
//

static void initPanel(TFT_CLASS *t) {
#  if TFT_DRIVER == TFT_DRIVER_ST7789
  t->init(TFT_W, TFT_H);
#  else
  t->begin();
#  endif
  t->setSPISpeed(TFT_SPI_HZ);
  t->setRotation(TFT_ROTATION);
}

template <typename SPI_T>
static void beginSpiPort(SPI_T &port, int sck, int mosi) {
#  if defined(ARDUINO_ARCH_RP2040)
  port.setSCK(sck);
  port.setTX(mosi);
  port.begin();
#  elif defined(ARDUINO_ARCH_ESP32)
  port.begin(sck, -1, mosi, -1);
#  else
  (void)sck;
  (void)mosi;
  port.begin();
#  endif
}

bool displayBegin(void) {
  beginSpiPort(TFT_SPI_PORT, TFT_SCK, TFT_MOSI);
#  if NUM_EYES > 1
  // Only if the second panel really is on a different bus; re-initialising the
  // same port would tear down the one just configured.
  const void *bus0 = (const void *)&TFT_SPI_PORT;
  const void *bus1 = (const void *)&TFT1_SPI_PORT;
  if (bus1 != bus0) beginSpiPort(TFT1_SPI_PORT, TFT1_SCK, TFT1_MOSI);
#  endif

  eyePanelReset(rstPin, NUM_EYES);
  for (int e = 0; e < NUM_EYES; e++) {
    initPanel(panel[e]);
    DBG("  panel %d initialised (CS=GPIO%d DC=GPIO%d)\n", e, csPin[e],
        dcPin[e]);
  }

#  if TFT_FAST_SPI_ACTIVE
  spiInst[0] = spiForPin(TFT_SCK);
  DBG("fast SPI: panel 0 on spi%d (SCK pin %d)\n", (spiInst[0] == spi0) ? 0 : 1,
      TFT_SCK);
#    if NUM_EYES > 1
  spiInst[1] = spiForPin(TFT1_SCK);
  DBG("fast SPI: panel 1 on spi%d (SCK pin %d)\n", (spiInst[1] == spi0) ? 0 : 1,
      TFT1_SCK);
#    endif

#    if TFT_DMA
  for (int e = 0; e < NUM_EYES; e++) {
    dmaCh[e] = dma_claim_unused_channel(false);
    if (dmaCh[e] < 0) {
      Serial.println("No free DMA channel; falling back to CPU bursts.");
    } else {
      dma_channel_config c = dma_channel_get_default_config(dmaCh[e]);
      channel_config_set_transfer_data_size(&c, DMA_SIZE_16);
      channel_config_set_read_increment(&c, true);
      channel_config_set_write_increment(&c, false);
      channel_config_set_dreq(&c, spi_get_dreq(spiInst[e], true));
      dma_channel_configure(dmaCh[e], &c, &spi_get_hw(spiInst[e])->dr, NULL, 0,
                            false);
      DBG("DMA: panel %d on channel %d\n", e, dmaCh[e]);
    }
    dmaActive[e] = false;
  }
#    endif
#  endif

  panelW = panel[0]->width();
  panelH = panel[0]->height();

#  if TFT_BACKLIGHT >= 0
  pinMode(TFT_BACKLIGHT, OUTPUT);
  digitalWrite(TFT_BACKLIGHT, HIGH);
#  endif

  DBG("TFT up: %d panel(s), %dx%d at %ld Hz\n", NUM_EYES, panelW, panelH,
      (long)TFT_SPI_HZ);
  return (panelW > 0) && (panelH > 0);
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

  // A partial stripe would leave the buffer rows non-contiguous, so pick the
  // widest stripe that divides the eye exactly.
  stripeW = 1;
  for (int w = TFT_STRIPE_COLS; w >= 1; w--) {
    if ((size % w) == 0) {
      stripeW = w;
      break;
    }
  }
  displayColumnStride = -stripeW;

  free(scratch);
  scratch = (uint16_t *)eyeMalloc((size_t)stripeW * size * COLUMN_BUFFERS *
                                  sizeof(uint16_t));
  scratchIdx = 0;
  stripeBase = 0;
  if (scratch) {
    DBG("  stripes: %d columns, %d windows per eye, %u bytes\n", stripeW,
        size / stripeW,
        (unsigned)((size_t)stripeW * size * COLUMN_BUFFERS * 2));
  }
  if (!scratch) Serial.println("Column buffer allocation failed!");
}

void displayClear(uint16_t color) {
  for (int e = 0; e < NUM_EYES; e++) panel[e]->fillScreen(color);
}

void displayFrameBegin(void) {}

// startWrite / endWrite bracket each eye rather than the whole frame,
void displayEyeBegin(int eye) {
  panel[eye]->startWrite();
}

void displayEyeEnd(int eye) {
#  if TFT_FAST_SPI_ACTIVE && TFT_DMA
  dmaSettle(eye); // The last column of the eye must land before CS releases
  spi_set_format(spiInst[eye], 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
#  endif
  panel[eye]->endWrite();
}

uint16_t *displayColumn(int eye, int x) {
  (void)eye;
  if (!scratch) return NULL;
  const int c = x % stripeW; // Column within the stripe
  uint16_t *base = &scratch[(size_t)scratchIdx * stripeW * eyeSize];
  if (c == 0) stripeBase = x;
  // Bottom row of this column; the renderer walks up by one stripe width.
  return &base[(size_t)(eyeSize - 1) * stripeW + c];
}

void displayColumnDone(int eye, int x) {
  if (!scratch) return;
  // Nothing leaves until the stripe is full.
  if (((x % stripeW) != (stripeW - 1)) && (x != eyeSize - 1)) return;

#  if PROFILE_FRAME
  uint32_t t0 = micros();
#  endif
  uint16_t *buf = &scratch[(size_t)scratchIdx * stripeW * eyeSize];
  const uint32_t count = (uint32_t)stripeW * eyeSize;

#  if TFT_FAST_SPI_ACTIVE && TFT_DMA
  dmaSettle(eye);
  spi_set_format(spiInst[eye], 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
  panel[eye]->setAddrWindow(originX + stripeBase, originY, stripeW, eyeSize);
  spi_set_format(spiInst[eye], 16, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
  dma_channel_transfer_from_buffer_now(dmaCh[eye], buf, count);
  dmaActive[eye] = true;

#  elif TFT_FAST_SPI_ACTIVE
  panel[eye]->setAddrWindow(originX + stripeBase, originY, stripeW, eyeSize);
  pushPixels(spiInst[eye], buf, count);

#  else
  panel[eye]->setAddrWindow(originX + stripeBase, originY, stripeW, eyeSize);
  panel[eye]->writePixels(buf, count, true, false);
#  endif

  scratchIdx ^= 1;
#  if PROFILE_FRAME
  displayBusyMicros += micros() - t0;
#  endif
}

void displayFrameEnd(void) {}

void displaySelfTest(void) {
  const uint16_t idColor[2] = {0xF800, 0x001F}; // panel 0 red, panel 1 blue
  const char *idName[2] = {"RED", "BLUE"};
  (void)idName;

  for (int e = 0; e < NUM_EYES; e++) {
    DBG("  self-test: panel %d -> %s   (CS=GPIO%d DC=GPIO%d RST=GPIO%d)\n", e,
        idName[e], csPin[e], dcPin[e], rstPin[e]);
    panel[e]->fillScreen(idColor[e]);
  }
  DBGLN("  if a panel stays dark, its CS/DC/RST GPIO numbers are wrong");
  delay(2500);

  const uint16_t bars[3] = {0x07E0, 0xFFFF, 0x0000};
  for (uint8_t i = 0; i < 3; i++) {
    for (int e = 0; e < NUM_EYES; e++) panel[e]->fillScreen(bars[i]);
    delay(400);
  }

  for (int e = 0; e < NUM_EYES; e++) {
    panel[e]->fillScreen(0);
    panel[e]->fillRect(e ? panelW / 2 : 0, 0, panelW / 2, panelH / 2, 0xFFE0);
  }
  DBGLN("  yellow block: TOP-LEFT on panel 0, TOP-RIGHT on panel 1");
  delay(1500);
}

#endif // EYE_DISPLAY == EYE_DISPLAY_TFT
