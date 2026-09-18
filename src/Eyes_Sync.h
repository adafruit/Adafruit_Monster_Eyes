/**
 * @file Eyes_Sync.h
 * @brief Keep two boards, one eye each, animating in step over I2C.
 *
 * Works with any display backend: these classes only drive the public control
 * API. Build both boards with a single eye and give them the same config and
 * assets. The primary animates and broadcasts; the secondary renders what
 * it is sent.
 *
 * WIRING: one STEMMA QT cable between the two boards. The primary is the
 * I2C controller and writes packets to the secondary's address.
 *
 * WHAT IS SENT: gaze target, pupil dilation, blink phase, and the primary's
 * clock so iris rotation stays in step.
 *
 * call begin() on the eyes FIRST. PicoDVI raises the system
 * clock when it starts, and Wire's clock divider is fixed from the peripheral
 * clock at begin() time.
 *
 * for Qualia, whose PCA9554A expander sits at 0x3F, the display has to be
 * brought up while Wire is still a controller.
 *
 * That second case needs one extra step for a SECONDARY, because one I2C
 * controller cannot be both a controller and a peripheral: bring the display
 * up, then release the bus before begin() here.
 *
 *     eyes.begin();
 *     // ...anything else that needs the expander, e.g. the backlight
 *     Wire.end();
 *     eyeSync.begin();
 *
 */

#ifndef _EYES_SYNC_H_
#define _EYES_SYNC_H_

#include "Adafruit_Monster_Eyes.h"
#include <Wire.h>

#define EYES_SYNC_MAGIC 0xA5 ///< First byte of a sync packet

// Spelled as a macro so doxygen can be told to expand it to nothing; it
// cannot parse a bare attribute on a struct.
#define EYES_PACKED __attribute__((packed)) ///< No padding between fields

/**
 * @brief State the two boards must agree on, sent once per frame.
 *
 * Deliberately small: gaze, pupil, blink phase and the primary's clock. Iris
 * rotation is derived from time rather than sent, so carrying the clock keeps
 * both eyes spinning in step.
 */
struct EyesSyncPacket {
  uint8_t magic; ///< Frame marker, EYES_SYNC_MAGIC
  int16_t eyeX;  ///< Gaze target in map pixels
  int16_t eyeY;  ///< Gaze target in map pixels
  uint16_t iris; ///< Iris fraction, 0-65535
  uint8_t blink; ///< Blink phase, 0 open to 255 shut
  uint32_t ms;   ///< Primary's millis(), so iris rotation stays in step
  uint8_t sum;   ///< XOR of every preceding byte
} EYES_PACKED;

/**
 * @brief Broadcasts this board's animation state to a secondary.
 *
 * Draws the character's RIGHT eye by default.
 */
class Eyes_SyncPrimary {
public:
  /**
   * @param eyes  The eye instance to read state from.
   * @param wire  I2C bus; the STEMMA QT port is the default Wire.
   * @param addr  Address the secondary answers to.
   * @param right true to draw the right eye, matching a two-eye build's eye 0.
   */
  Eyes_SyncPrimary(Adafruit_Monster_Eyes &eyes, TwoWire &wire = Wire,
                   uint8_t addr = 0x42, bool right = true);

  /**
   * @brief Become the I2C controller. Call AFTER the eyes' begin().
   * @param hz Bus speed; a packet is 13 bytes, so 400 kHz is plenty.
   * @return true always; failures show up as unacknowledged packets.
   */
  bool begin(uint32_t hz = 400000);

  /** @brief Send this frame's state. Call once per frame, after draw(). */
  void send(void);

  /** @brief Packets acknowledged since the last count reset. @return Count. */
  uint32_t sent(void) const { return _sent; }
  /** @brief Packets the secondary did not acknowledge. @return Count. */
  uint32_t failed(void) const { return _bad; }
  /** @brief Zero the counters. */
  void resetCounts(void) { _sent = _bad = 0; }

private:
  Adafruit_Monster_Eyes &_eyes; ///< Eye instance being followed
  TwoWire &_wire;               ///< I2C bus
  uint8_t _addr;                ///< Secondary's address
  uint32_t _sent;               ///< Packets acknowledged
  uint32_t _bad;                ///< Packets not acknowledged
};

/**
 * @brief Renders the state a primary sends it.
 *
 * Draws the character's LEFT eye by default. While packets are arriving the
 * gaze, pupil and lids are owned by the link; when they stop, they are handed
 * back to the local animators.
 */
class Eyes_SyncSecondary {
public:
  /**
   * @param eyes  The eye instance to drive.
   * @param wire  I2C bus; the STEMMA QT port is the default Wire.
   * @param addr  Address to answer to.
   * @param right false to draw the left eye, matching a two-eye build's eye 1.
   */
  Eyes_SyncSecondary(Adafruit_Monster_Eyes &eyes, TwoWire &wire = Wire,
                     uint8_t addr = 0x42, bool right = false);

  /**
   * @brief Become an I2C peripheral. Call AFTER the eyes' begin().
   * @return true always.
   */
  bool begin(void);

  /**
   * @brief Consume any waiting packets. Call once per frame, before update().
   *
   * Reads everything available rather than one packet per frame, so a
   * secondary rendering slower than the primary tracks the latest state
   * instead of falling progressively further behind.
   *
   * @return true if a good packet arrived recently enough to trust.
   */
  bool poll(void);

  /**
   * @brief How long silence lasts before the eye animates on its own.
   * @param ms Milliseconds.
   */
  void setTimeout(uint32_t ms) { _timeoutMs = ms; }

  /** @brief Is the link currently carrying state? @return true when locked. */
  bool live(void) const { return _live; }
  /** @brief Valid packets received. @return Count. */
  uint32_t good(void) const { return _good; }
  /** @brief Packets dropped on a bad checksum or length. @return Count. */
  uint32_t bad(void) const { return _bad; }
  /** @brief Raw bytes seen on the link. @return Count. */
  uint32_t bytes(void) const { return _bytes; }
  /** @brief Zero the counters. */
  void resetCounts(void) { _good = _bad = _bytes = 0; }

  /**
   * @brief Print a one-line link diagnosis, with a hint when something is
   *        wrong.
   * @param stream Where to write.
   */
  void report(Stream &stream);

private:
  void apply(const EyesSyncPacket &p);

  Adafruit_Monster_Eyes &_eyes; ///< Eye instance being driven
  TwoWire &_wire;               ///< I2C bus
  uint8_t _addr;                ///< Our address
  uint32_t _timeoutMs;          ///< Silence before free-running
  uint32_t _lastGood;           ///< millis() of the last valid packet
  bool _live;                   ///< Packets are arriving
  uint32_t _good;               ///< Valid packets
  uint32_t _bad;                ///< Rejected packets
  uint32_t _bytes;              ///< Raw bytes seen
};

#endif // _EYES_SYNC_H_
