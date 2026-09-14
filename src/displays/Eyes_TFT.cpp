/**
 * @file Eyes_TFT.cpp
 * @brief Display backend: SPI TFT panels driven by Adafruit_GFX.
 */

#include "Eyes_TFT.h"
#include <Adafruit_SPITFT.h>

#if defined(ARDUINO_ARCH_RP2040)
#include <hardware/dma.h>
#include <hardware/spi.h>

/// Which SPI block a pin belongs to on RP2: pins alternate in blocks of 8.
static spi_inst_t *spiForPin(int sck) {
  return (((sck / 8) % 2) == 0) ? spi0 : spi1;
}

/// CPU-driven 16-bit burst, for when no DMA channel was available.
static void pushPixels(spi_inst_t *spi, const uint16_t *buf, size_t n) {
  while (spi_is_busy(spi))
    tight_loop_contents(); // let commands finish
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
  if (guard >= guardMax) {
    static bool warned = false;
    if (!warned) {
      warned = true;
      EYES_ERR("SPI burst stalled -- wrong SPI block? Check setFastSPI().\n");
    }
  }
  while (spi_is_busy(spi))
    tight_loop_contents();
  spi_set_format(spi, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
}
#endif // ARDUINO_ARCH_RP2040

Eyes_TFT::Eyes_TFT(Adafruit_SPITFT *panel0, Adafruit_SPITFT *panel1)
    : Eyes_StripeDisplay(panel1 ? 2 : 1), _fast(false), _dma(true) {
  _panel[0] = panel0;
  _sck[0] = -1;
#if MONSTER_EYES_MAX_EYES > 1
  _panel[1] = panel1;
  _sck[1] = -1;
#endif
#if defined(ARDUINO_ARCH_RP2040)
  for (int e = 0; e < MONSTER_EYES_MAX_EYES; e++) {
    _spiInst[e] = NULL;
    _dmaCh[e] = -1;
    _dmaActive[e] = false;
  }
#endif
}

Eyes_TFT::~Eyes_TFT() {}

void Eyes_TFT::setFastSPI(int sck0, int sck1, bool useDma) {
  _fast = true;
  _dma = useDma;
  _sck[0] = sck0;
#if MONSTER_EYES_MAX_EYES > 1
  _sck[1] = (sck1 >= 0) ? sck1 : sck0;
#else
  (void)sck1;
#endif
}

int Eyes_TFT::buffersWanted(void) {
#if defined(ARDUINO_ARCH_RP2040)
  // Two buffers only pay off when the transfer is asynchronous: the renderer
  // fills one while DMA is still draining the other.
  if (_fast && _dma && (_dmaCh[0] >= 0))
    return 2;
#endif
  return 1;
}

void Eyes_TFT::panelSize(int *w, int *h) {
  *w = _panel[0] ? _panel[0]->width() : 0;
  *h = _panel[0] ? _panel[0]->height() : 0;
}

bool Eyes_TFT::begin(void) {
  if (!_panel[0]) {
    EYES_ERR("Eyes_TFT: no panel given\n");
    return false;
  }

  panelSize(&_panelW, &_panelH);
  if ((_panelW <= 0) || (_panelH <= 0)) {
    EYES_ERR("Eyes_TFT: panel reports 0x0 -- did you init() it before "
             "begin()?\n");
    return false;
  }

#if defined(ARDUINO_ARCH_RP2040)
  if (_fast) {
    for (int e = 0; e < _numEyes; e++) {
      spi_inst_t *inst = spiForPin(_sck[e]);
      _spiInst[e] = (void *)inst;
      EYES_DBG("  fast SPI: panel %d on spi%d (SCK pin %d)\n", e,
               (inst == spi0) ? 0 : 1, _sck[e]);
    }
    if (_dma) {
      for (int e = 0; e < _numEyes; e++) {
        _dmaCh[e] = dma_claim_unused_channel(false);
        if (_dmaCh[e] < 0) {
          EYES_ERR("No free DMA channel; falling back to CPU bursts.\n");
        } else {
          spi_inst_t *inst = (spi_inst_t *)_spiInst[e];
          dma_channel_config c = dma_channel_get_default_config(_dmaCh[e]);
          channel_config_set_transfer_data_size(&c, DMA_SIZE_16);
          channel_config_set_read_increment(&c, true);
          channel_config_set_write_increment(&c, false);
          channel_config_set_dreq(&c, spi_get_dreq(inst, true));
          dma_channel_configure(_dmaCh[e], &c, &spi_get_hw(inst)->dr, NULL, 0,
                                false);
          EYES_DBG("  DMA: panel %d on channel %d\n", e, _dmaCh[e]);
        }
        _dmaActive[e] = false;
      }
    }
  }
#endif

  EYES_DBG("TFT up: %d panel(s), %dx%d\n", _numEyes, _panelW, _panelH);
  return true;
}

#if defined(ARDUINO_ARCH_RP2040)
void Eyes_TFT::dmaSettle(int eye) {
  if (!_dmaActive[eye])
    return;
  uint32_t guard = 0;
  while (dma_channel_is_busy(_dmaCh[eye]) && (++guard < 5000000))
    tight_loop_contents();
  if (guard >= 5000000) {
    static bool warned = false;
    if (!warned) {
      warned = true;
      EYES_ERR("DMA stalled -- try setFastSPI(pin, -1, false).\n");
    }
    dma_channel_abort(_dmaCh[eye]);
  }
  _dmaActive[eye] = false;
  // The channel is done feeding the FIFO; the shifter may still be draining.
  while (spi_is_busy((spi_inst_t *)_spiInst[eye]))
    tight_loop_contents();
}
#endif

void Eyes_TFT::clear(uint16_t color) {
  for (int e = 0; e < _numEyes; e++)
    if (_panel[e])
      _panel[e]->fillScreen(color);
}

// startWrite / endWrite bracket each eye rather than the whole frame, so the
// bus is released between eyes on a shared bus.
void Eyes_TFT::eyeBegin(int eye) {
  if (_panel[eye])
    _panel[eye]->startWrite();
}

void Eyes_TFT::eyeEnd(int eye) {
#if defined(ARDUINO_ARCH_RP2040)
  if (_fast && (_dmaCh[eye] >= 0)) {
    dmaSettle(eye); // The last column must land before CS releases
    spi_set_format((spi_inst_t *)_spiInst[eye], 8, SPI_CPOL_0, SPI_CPHA_0,
                   SPI_MSB_FIRST);
  }
#endif
  if (_panel[eye])
    _panel[eye]->endWrite();
}

void Eyes_TFT::flushStripe(int eye, int x0, int width, uint16_t *buf) {
  Adafruit_SPITFT *p = _panel[eye];
  if (!p)
    return;
  const uint32_t count = (uint32_t)width * _eyeSize;

#if defined(ARDUINO_ARCH_RP2040)
  if (_fast && (_dmaCh[eye] >= 0)) {
    spi_inst_t *inst = (spi_inst_t *)_spiInst[eye];
    dmaSettle(eye);
    // The address window is command traffic: 8-bit frames. Pixels are 16.
    spi_set_format(inst, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    p->setAddrWindow(_originX + x0, _originY, width, _eyeSize);
    spi_set_format(inst, 16, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    dma_channel_transfer_from_buffer_now(_dmaCh[eye], buf, count);
    _dmaActive[eye] = true;
    return;
  }
  if (_fast) {
    p->setAddrWindow(_originX + x0, _originY, width, _eyeSize);
    pushPixels((spi_inst_t *)_spiInst[eye], buf, count);
    return;
  }
#endif

  p->setAddrWindow(_originX + x0, _originY, width, _eyeSize);
  p->writePixels(buf, count, true, false);
}

void Eyes_TFT::selfTest(void) {
  const uint16_t idColor[2] = {0xF800, 0x001F}; // panel 0 red, panel 1 blue
  const char *idName[2] = {"RED", "BLUE"};
  (void)idName;

  for (int e = 0; e < _numEyes; e++) {
    EYES_DBG("  self-test: panel %d -> %s\n", e, idName[e]);
    if (_panel[e])
      _panel[e]->fillScreen(idColor[e]);
  }
  EYES_DBG("  if a panel stays dark, check its CS/DC/RST pins\n");
  delay(2500);

  const uint16_t bars[3] = {0x07E0, 0xFFFF, 0x0000};
  for (uint8_t i = 0; i < 3; i++) {
    for (int e = 0; e < _numEyes; e++)
      if (_panel[e])
        _panel[e]->fillScreen(bars[i]);
    delay(400);
  }

  for (int e = 0; e < _numEyes; e++) {
    if (!_panel[e])
      continue;
    _panel[e]->fillScreen(0);
    _panel[e]->fillRect(e ? _panelW / 2 : 0, 0, _panelW / 2, _panelH / 2,
                        0xFFE0);
  }
  EYES_DBG("  yellow block: TOP-LEFT on panel 0, TOP-RIGHT on panel 1\n");
  delay(1500);
}
