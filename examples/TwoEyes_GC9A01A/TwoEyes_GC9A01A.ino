// Adafruit Monster Eyes -- two GC9A01A displays
//
// Eye appearance comes from config.eye and BMP files on the CIRCUITPY drive.

#include <Adafruit_Monster_Eyes.h>
#if !defined(ARDUINO_ARCH_ESP32)
#include <Adafruit_GC9A01A.h>
#endif

#define TFT_SCK SCK
#define TFT_MOSI MOSI
#define TFT_DC 9   // Shared by both panels
#define TFT_RST 10 // Shared; -1 if tied to the board's reset
#define TFT_CS 11  // Panel 0
#if defined(ARDUINO_ADAFRUIT_METRO_RP2350)
#define TFT1_CS 22 // Panel 1
#else
#define TFT1_CS 12 // Panel 1
#endif

#define TFT_W 240
#define TFT_H 240 // 320 for a 240x320 ST7789

#if defined(ARDUINO_ARCH_ESP32)
// esp_lcd drives the controller directly, so there is no panel object.
Eyes_ESPLCD display(EYES_ESPLCD_GC9A01A, TFT_W, TFT_H, TFT_SCK, TFT_MOSI,
                    TFT_CS, TFT_DC, TFT_RST, TFT1_CS);
Adafruit_Monster_Eyes eyes(&display);
#else
Adafruit_GC9A01A panel0(&SPI, TFT_DC, TFT_CS, -1);
Adafruit_GC9A01A panel1(&SPI, TFT_DC, TFT1_CS, -1);
Adafruit_Monster_Eyes eyes(&panel0, &panel1);
#endif

void setup() {
  Serial.begin(115200);
  // eyes.setVerbose(Serial); // debug
  eyes.setSelfTest(true);  // Panel 0 fills red, panel 1 blue

#if defined(ARDUINO_ARCH_ESP32)
  display.setSPISpeed(40000000);
  display.setOrientation(true /* invert */);
#else
  SPI.setSCK(TFT_SCK);
  SPI.setTX(TFT_MOSI);
  SPI.begin();
  eyes.setFastSPI(TFT_SCK);

  Adafruit_Monster_Eyes::resetPanels(TFT_RST);
  panel0.begin();
  panel1.begin();
  panel0.setSPISpeed(40000000);
  panel1.setSPISpeed(40000000);
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
