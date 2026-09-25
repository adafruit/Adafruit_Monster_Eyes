// Adafruit Monster Eyes -- slide the eyeball up off the display, then bring
// it back up from the bottom.

#include <Adafruit_GC9A01A.h>
#include <Adafruit_Monster_Eyes.h>

#define TFT_SCK SCK
#define TFT_MOSI MOSI
#if defined(ARDUINO_ADAFRUIT_QTPY_RP2040)    // EYESPI BFF
#define TFT_CS PIN_SERIAL2_TX                // GPIO 20, the pad marked TX
#define TFT_DC PIN_SERIAL2_RX                // GPIO 5, the pad marked RX
#define TFT_RST -1                           // Not wired on the BFF
#elif defined(ARDUINO_ADAFRUIT_QTPY_ESP32S2) // EYESPI BFF
#define TFT_CS TX                            // GPIO 5, the pad marked TX
#define TFT_DC RX                            // GPIO 16, the pad marked RX
#define TFT_RST -1                           // Not wired on the BFF
#else                                        // Feather and Metro wiring
#define TFT_CS 11
#define TFT_DC 9
#define TFT_RST 10 // -1 if the panel's reset is tied to the board's
#endif

#define ROLL_EVERY 5000  // Milliseconds between rolls
#define ROLL_OUT_MS 1000  // Time to slide up out of view
#define ROLL_HOLD_MS 250 // Time spent gone
#define ROLL_IN_MS 850   // Time to rise back into view

Adafruit_GC9A01A panel0(&SPI, TFT_DC, TFT_CS, -1);
Adafruit_Monster_Eyes eyes(&panel0);

enum RollState { IDLE, ROLL_AWAY, GONE, ROLL_BACK };
RollState rollState = IDLE;
uint32_t rollStart = 0;
int rollSpan = 0; // Offset at which the eye is fully out of sight

// Ease in and out
static float ease(float t) {
  if (t < 0.0f)
    t = 0.0f;
  else if (t > 1.0f)
    t = 1.0f;
  return 3.0f * t * t - 2.0f * t * t * t;
}

void setup() {
  Serial.begin(115200);
  // eyes.setVerbose(Serial);

  SPI.begin();
  Adafruit_Monster_Eyes::resetPanels(TFT_RST);
  panel0.begin();
  panel0.setSPISpeed(40000000);

  if (!eyes.begin()) {
    Serial.print("Monster Eyes failed: ");
    Serial.println(eyes.errorString());
    while (1)
      delay(1000);
  }

  rollSpan = eyes.eyeSize();
  rollStart = millis();
}

void loop() {
  const uint32_t now = millis();
  const uint32_t since = now - rollStart;

  switch (rollState) {
  case IDLE:
    if (since >= ROLL_EVERY) {
      rollState = ROLL_AWAY;
      rollStart = now;
    }
    break;

  case ROLL_AWAY: // Up and out the top
    eyes.setDrawOffset(0, (int)(rollSpan * ease((float)since / ROLL_OUT_MS)));
    if (since >= ROLL_OUT_MS) {
      rollState = GONE;
      rollStart = now;
    }
    break;

  case GONE:
    eyes.setDrawOffset(0, -rollSpan);
    if (since >= ROLL_HOLD_MS) {
      rollState = ROLL_BACK;
      rollStart = now;
    }
    break;

  case ROLL_BACK: // Rise into view from the bottom
    eyes.setDrawOffset(
        0, (int)(-rollSpan * (1.0f - ease((float)since / ROLL_IN_MS))));
    if (since >= ROLL_IN_MS) {
      eyes.setDrawOffset(0, 0); // Exactly centred again
      rollState = IDLE;
      rollStart = now;
    }
    break;
  }

  eyes.animate();
}
