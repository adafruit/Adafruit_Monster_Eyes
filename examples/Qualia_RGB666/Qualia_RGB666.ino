// Adafruit Monster Eyes -- one eye on an Adafruit Qualia ESP32-S3.
//
// Eye appearance comes from config.eye and BMP files on the CIRCUITPY drive.

#include <Adafruit_Monster_Eyes.h>
#include <Arduino_GFX_Library.h>

#define RGB_W 480
#define RGB_H 480
#define EYE_SCALE 2

// 2.1" round display
Arduino_XCA9554SWSPI expander(PCA_TFT_RESET, PCA_TFT_CS, PCA_TFT_SCK,
                              PCA_TFT_MOSI, &Wire, 0x3F);

Arduino_ESP32RGBPanel rgbpanel(
    TFT_DE, TFT_VSYNC, TFT_HSYNC, TFT_PCLK, TFT_R1, TFT_R2, TFT_R3, TFT_R4,
    TFT_R5, TFT_G0, TFT_G1, TFT_G2, TFT_G3, TFT_G4, TFT_G5, TFT_B1, TFT_B2,
    TFT_B3, TFT_B4, TFT_B5,
    1 /* HSYNC polarity */, 50 /* front porch */, 2 /* pulse width */,
    44 /* back porch */,
    1 /* VSYNC polarity */, 16 /* front porch */, 2 /* pulse width */,
    18 /* back porch */);

Arduino_RGB_Display gfx(
// 2.1" 480x480 round display
  RGB_W, RGB_H, &rgbpanel, 0 /* rotation */, true /* auto_flush */,
  &expander, GFX_NOT_DEFINED /* RST */, TL021WVC02_init_operations, sizeof(TL021WVC02_init_operations));

// 2.8" 480x480 round display
//    480 /* width */, 480 /* height */, &rgbpanel, 0 /* rotation */, true /* auto_flush */,
//    &expander, GFX_NOT_DEFINED /* RST */, TL028WVC01_init_operations, sizeof(TL028WVC01_init_operations));

// 4.0" 720x720 round display
//    720 /* width */, 720 /* height */, &rgbpanel, 0 /* rotation */, true /* auto_flush */,
//    &expander, GFX_NOT_DEFINED /* RST */, hd40015c40_init_operations, sizeof(hd40015c40_init_operations));
// needs also the rgbpanel to have these pulse/sync values:
//    1 /* hync_polarity */, 46 /* hsync_front_porch */, 2 /* hsync_pulse_width */, 44 /* hsync_back_porch */,
//    1 /* vsync_polarity */, 50 /* vsync_front_porch */, 16 /* vsync_pulse_width */, 16 /* vsync_back_porch */

Adafruit_Monster_Eyes eyes(&gfx, EYE_SCALE);

void setup() {
  Serial.begin(115200);
  eyes.setVerbose(Serial);
  Wire.setClock(1000000);

  if (!eyes.begin()) {
    Serial.print("Monster Eyes failed: ");
    Serial.println(eyes.errorString());
    while (1)
      delay(1000);
  }

  expander.pinMode(PCA_TFT_BACKLIGHT, OUTPUT);
  expander.digitalWrite(PCA_TFT_BACKLIGHT, HIGH);
}

void loop() {
  eyes.animate();

  static uint32_t last = 0;
  if (millis() - last >= 1000) {
    last = millis();
    Serial.printf("%.0f fps\n", eyes.frameRate());
  }
}
