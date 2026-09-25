// Adafruit Monster Eyes -- swap the whole eye on a button press.

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

#define BUTTON_PIN 21 // boot button on qt py rp2040

// update for config.eye files on your board
// should be in the base directory of CIRCUITPY
const char *eyeFiles[] = {
    "/fish.eye",
    "/hazel.eye",
    "/demon.eye",
};
const uint8_t eyeCount = sizeof(eyeFiles) / sizeof(eyeFiles[0]);
uint8_t eyeIndex = 0;

Adafruit_GC9A01A panel0(&SPI, TFT_DC, TFT_CS, -1);
Adafruit_Monster_Eyes eyes(&panel0);

void setup() {
  Serial.begin(115200);
  // eyes.setVerbose(Serial);

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  SPI.begin();
  Adafruit_Monster_Eyes::resetPanels(TFT_RST);
  panel0.begin();
  panel0.setSPISpeed(40000000);
  eyes.keepStorageMounted(true);

  eyes.setConfigFile(eyeFiles[eyeIndex]);

  if (!eyes.begin()) {
    Serial.print("Monster Eyes failed: ");
    Serial.println(eyes.errorString());
    while (1)
      delay(1000);
  }
}

bool buttonPressed() {
  static bool wasDown = false;
  static uint32_t lastChange = 0;

  const bool down = (digitalRead(BUTTON_PIN) == LOW);
  if (down == wasDown)
    return false;
  if ((millis() - lastChange) < 40)
    return false;

  lastChange = millis();
  wasDown = down;
  return down;
}

void loop() {
  if (buttonPressed()) {
    eyeIndex = (eyeIndex + 1) % eyeCount;
    Serial.printf("\nButton: loading %s\n", eyeFiles[eyeIndex]);

    const uint32_t t0 = millis();
    if (!eyes.loadEye(eyeFiles[eyeIndex])) {
      Serial.print("  failed: ");
      Serial.println(eyes.errorString());
    } else {
      Serial.printf("  loaded in %lu ms\n", millis() - t0);
    }
    return;
  }

  eyes.animate();
}
