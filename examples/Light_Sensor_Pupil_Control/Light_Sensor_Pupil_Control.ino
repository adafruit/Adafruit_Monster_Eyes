// Pupil Control with a light sensor
//
// Use a TSL2585 to control the pupil size of the eye as it animates

#include <Adafruit_TSL2585.h>
#include <Adafruit_Monster_Eyes.h>

Adafruit_TSL2585 tsl2585;

#if !defined(ARDUINO_ARCH_ESP32)
#include <Adafruit_GC9A01A.h>
#endif

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

#define TFT_W 240
#define TFT_H 240 // 320 for a 240x320 ST7789

#if defined(ARDUINO_ARCH_ESP32)
// esp_lcd drives the controller directly, so there is no panel object.
Eyes_ESPLCD display(EYES_ESPLCD_GC9A01A, TFT_W, TFT_H, TFT_SCK, TFT_MOSI,
                    TFT_CS, TFT_DC, TFT_RST);
Adafruit_Monster_Eyes eyes(&display);
#else
Adafruit_GC9A01A panel0(&SPI, TFT_DC, TFT_CS, -1);
Adafruit_Monster_Eyes eyes(&panel0);
#endif

#define PHOTO_DARK 10.0f
#define PHOTO_BRIGHT 2000.0f

void setup() {
  Serial.begin(115200);
  // eyes.setVerbose(Serial); // debug

#if defined(ARDUINO_ARCH_ESP32)
  display.setSPISpeed(40000000);
  display.setOrientation(true /* invert */);
#else
  SPI.setSCK(TFT_SCK);
  SPI.setTX(TFT_MOSI);
  SPI.begin();
  eyes.setFastSPI(TFT_SCK); // Batched SPI + DMA
  Adafruit_Monster_Eyes::resetPanels(TFT_RST);
  panel0.begin();
  panel0.setSPISpeed(40000000);
#endif

  if (!eyes.begin()) {
    Serial.print("Monster Eyes failed: ");
    Serial.println(eyes.errorString());
    while (1)
      delay(1000);
  }
  delay(250);

  if (!tsl2585.begin()) {
  // if (!tsl2585.begin(TSL2585_DEFAULT_ADDR, &Wire1)) { // QT Py RP2040 STEMMA QT port
    Serial.println("Could not find a TSL2585. Check the wiring and I2C address.");
    while (true) {
      delay(10);
    }
  }
  if (!tsl2585.setIntegrationTime(50)) {
    Serial.println("Could not set the integration time.");
    while (true) {
      delay(10);
    }
  }
}

void loop() {
  static float smoothed = 0.5f;

  if (tsl2585.dataReady()) {
    tsl2585_data_t data;
    if (tsl2585.readData(&data)) {
      float photo = data.photopic_1x;
      float target = mapFloat(photo, PHOTO_DARK, PHOTO_BRIGHT, 1.0f, 0.0f);
      smoothed += (target - smoothed) * 0.15f;
      eyes.setPupil(smoothed);
    }
  }

  eyes.animate();

  static uint32_t last = 0;
  if (millis() - last >= 1000) {
    last = millis();
    Serial.printf("%.0f fps, pupil %.2f\n", eyes.frameRate(), smoothed);
  }
}

float mapFloat(float x, float inMin, float inMax,
                      float outMin, float outMax) {
  if (inMax == inMin) return outMin;
  float t = (x - inMin) / (inMax - inMin);
  if (t < 0.0f) t = 0.0f;
  else if (t > 1.0f) t = 1.0f;
  return outMin + t * (outMax - outMin);
}