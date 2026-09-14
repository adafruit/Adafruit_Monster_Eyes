// Adafruit Monster Eyes -- one eye over PicoDVI
//
// Eye appearance comes from config.eye and BMP files on the CIRCUITPY drive.

#include <Adafruit_Monster_Eyes.h>
#include <PicoDVI.h>

#if defined(ARDUINO_ADAFRUIT_FEATHER_RP2040_DVI)
#define DVI_PIN_CONFIG adafruit_feather_dvi_cfg
#else
#define DVI_PIN_CONFIG adafruit_dvibell_cfg // PiCowbell DVI;
                                            // pico_sock_cfg // Pico DVI Sock
                                            // pimoroni_demo_hdmi_cfg // Pimoroni Pico DV
#endif

DVIGFX16 dvi(DVI_RES_320x240p60, DVI_PIN_CONFIG);

Adafruit_Monster_Eyes eyes(&dvi, 1);

void setup() {
  Serial.begin(115200);
  // eyes.setVerbose(Serial); // debug

  if (eyes.driveModeRequested()) {
    eyes.runDriveMode();
  }
  eyes.setDriveModeEnabled(false);

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
