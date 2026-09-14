// Adafruit Monster Eyes -- one eye on ST7789
//
// Eye appearance comes from config.eye and BMP files on the CIRCUITPY drive.

#include <Adafruit_Monster_Eyes.h>
#if !defined(ARDUINO_ARCH_ESP32)
#include <Adafruit_ST7789.h>
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
Eyes_ESPLCD display(EYES_ESPLCD_ST7789, TFT_W, TFT_H, TFT_SCK, TFT_MOSI, TFT_CS,
                    TFT_DC, TFT_RST);
Adafruit_Monster_Eyes eyes(&display);
#else
Adafruit_ST7789 panel0(&SPI, TFT_CS, TFT_DC, -1);
Adafruit_Monster_Eyes eyes(&panel0);
#endif

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
  panel0.init(TFT_W, TFT_H);
  panel0.setRotation(2);
  panel0.setSPISpeed(40000000);
#endif

  if (!eyes.begin()) {
    Serial.print("Monster Eyes failed: ");
    Serial.println(eyes.errorString());
    while (1)
      delay(1000);
  }
}

void loop() {
  eyes.animate();

  static uint32_t last = 0;
  if (millis() - last >= 1000) {
    last = millis();
    Serial.printf("%.0f fps\n", eyes.frameRate());
  }
}
