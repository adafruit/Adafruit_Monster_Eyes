/**
 * @file Eyes_Sync.cpp
 * @brief Two-board sync over I2C, as a client of the public control API.
 */

#include "Eyes_Sync.h"
#include <math.h>
#include <string.h>

/** @brief XOR checksum over a packet's leading bytes. */
static uint8_t syncChecksum(const uint8_t *p) {
  uint8_t x = 0;
  for (size_t i = 0; i < sizeof(EyesSyncPacket) - 1; i++)
    x ^= p[i];
  return x;
}

// ===========================================================================
//  PRIMARY
// ===========================================================================

Eyes_SyncPrimary::Eyes_SyncPrimary(Adafruit_Monster_Eyes &eyes, TwoWire &wire,
                                   uint8_t addr, bool right)
    : _eyes(eyes), _wire(wire), _addr(addr), _sent(0), _bad(0) {
  // Telling the eye which side it is also switches on the toe-in a two-eye
  // build would have, and picks the config block it reads.
  _eyes.setSide(right);
}

bool Eyes_SyncPrimary::begin(uint32_t hz) {
  _wire.begin(); // Controller
  _wire.setClock(hz);
  EYES_DBG("sync: PRIMARY writing to I2C 0x%02X at %lu Hz, clk %lu Hz\n", _addr,
           (unsigned long)hz, (unsigned long)eyesCpuHz());
  EYES_DBG("sync: drawing the %s eye\n", _eyes.side() ? "RIGHT" : "LEFT");
  return true;
}

void Eyes_SyncPrimary::send(void) {
  EyesSyncPacket p;
  p.magic = EYES_SYNC_MAGIC;
  p.eyeX = (int16_t)lroundf(_eyes.gazeMapX());
  p.eyeY = (int16_t)lroundf(_eyes.gazeMapY());
  float iris = _eyes.irisFraction();
  if (iris < 0.0f)
    iris = 0.0f;
  else if (iris > 1.0f)
    iris = 1.0f;
  p.iris = (uint16_t)(iris * 65535.0f);
  float blink = _eyes.blinkPhase();
  if (blink < 0.0f)
    blink = 0.0f;
  else if (blink > 1.0f)
    blink = 1.0f;
  p.blink = (uint8_t)(blink * 255.0f);
  p.ms = millis();
  p.sum = syncChecksum((const uint8_t *)&p);

  _wire.beginTransmission(_addr);
  _wire.write((const uint8_t *)&p, sizeof(p));
  // Non-zero means the secondary did not acknowledge.
  if (_wire.endTransmission() != 0)
    _bad++;
  else
    _sent++;
}

// ===========================================================================
//  SECONDARY
// ===========================================================================

// The receive callback is a plain function, so it needs a way back to the
// object. Single instance, like the rest of the library.
static Eyes_SyncSecondary *s_secondary = NULL;
static TwoWire *s_wire = NULL;
static volatile uint8_t s_buf[sizeof(EyesSyncPacket)];
static volatile bool s_have = false;
static volatile uint32_t s_bytes = 0;
static volatile uint32_t s_rejected = 0;

/**
 * @brief I2C receive interrupt: latch one packet and return.
 *
 * Runs in interrupt context, so it does nothing but copy. Validation and the
 * animation update happen in poll() on the main loop.
 *
 * @param n Bytes the controller wrote.
 */
static void syncOnReceive(int n) {
  if (!s_wire)
    return;
  if (n != (int)sizeof(EyesSyncPacket)) {
    while (s_wire->available())
      s_wire->read();
    s_rejected++;
    return;
  }
  for (size_t i = 0; i < sizeof(EyesSyncPacket); i++)
    s_buf[i] = (uint8_t)s_wire->read();
  s_bytes += (uint32_t)n;
  s_have = true;
}

Eyes_SyncSecondary::Eyes_SyncSecondary(Adafruit_Monster_Eyes &eyes,
                                       TwoWire &wire, uint8_t addr, bool right)
    : _eyes(eyes), _wire(wire), _addr(addr), _timeoutMs(500), _lastGood(0),
      _live(false), _good(0), _bad(0), _bytes(0) {
  _eyes.setSide(right);
}

bool Eyes_SyncSecondary::begin(void) {
  s_secondary = this;
  s_wire = &_wire;
  _wire.begin(_addr); // Peripheral
  _wire.onReceive(syncOnReceive);
  EYES_DBG("sync: SECONDARY answering I2C 0x%02X, clk %lu Hz\n", _addr,
           (unsigned long)eyesCpuHz());
  EYES_DBG("sync: drawing the %s eye\n", _eyes.side() ? "RIGHT" : "LEFT");
  return true;
}

void Eyes_SyncSecondary::apply(const EyesSyncPacket &p) {
  // Each of these takes ownership of one attribute, which is exactly what a
  // sketch driving the eye from a sensor would do.
  _eyes.setGazeMap((float)p.eyeX, (float)p.eyeY);
  _eyes.setIrisFraction((float)p.iris / 65535.0f);
  _eyes.setBlink((float)p.blink / 255.0f);
  _eyes.setTimeOffset((int32_t)p.ms - (int32_t)millis());
}

bool Eyes_SyncSecondary::poll(void) {
  // The interrupt latches a whole packet or nothing.
  if (s_have) {
    EyesSyncPacket p;
    noInterrupts();
    memcpy(&p, (const void *)s_buf, sizeof(p));
    s_have = false;
    interrupts();
    if ((p.magic == EYES_SYNC_MAGIC) &&
        (p.sum == syncChecksum((const uint8_t *)&p))) {
      _good++;
      apply(p);
      _lastGood = millis();
    } else {
      _bad++;
    }
  }
  _bytes += s_bytes;
  s_bytes = 0;
  _bad += s_rejected;
  s_rejected = 0;

  const bool wasLive = _live;
  _live = (_lastGood != 0) && ((millis() - _lastGood) < _timeoutMs);

  // A pulled cable should leave a working eye, not a frozen one: hand every
  // attribute back to the local animators.
  if (wasLive && !_live) {
    _eyes.releaseGaze();
    _eyes.releasePupil();
    _eyes.releaseBlink();
    _eyes.setTimeOffset(0);
    EYES_DBG("sync: link lost -- animating locally\n");
  }
  return _live;
}

void Eyes_SyncSecondary::report(Stream &stream) {
  stream.printf("sync: %s, %lu good, %lu bad, %lu bytes\n",
                _live ? "LOCKED" : "free-running", (unsigned long)_good,
                (unsigned long)_bad, (unsigned long)_bytes);
  // Only comment when something is actually wrong. LOCKED with good packets
  // needs no advice.
  if (!_live) {
    if (_bytes == 0) {
      stream.println("      no bytes -- check the cable and a common ground");
    } else if (_good == 0) {
      stream.println("      bytes but no valid packets -- do both boards "
                     "report the same clk?");
    }
  } else if (_bad > _good / 8) {
    stream.println("      many corrupt packets -- shorten the wire or drop "
                   "the bus speed");
  }
}
