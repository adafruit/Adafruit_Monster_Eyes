// Adafruit Monster Eyes -- primary for PicoDVI sync
//
// Upload PicoDVI_Sync_Primary to one board and PicoDVI_Sync_Secondary to the
// other, with the same config.eye and assets on both. 
//
// WIRING: one STEMMA QT cable between the two boards or connect SDA to SDA, SCL to SCL

#include <Adafruit_Monster_Eyes.h>
#include <Eyes_Sync.h>
#include <PicoDVI.h>

#if defined(ARDUINO_ADAFRUIT_FEATHER_RP2040_DVI)
#define DVI_CFG adafruit_feather_dvi_cfg
#else
#define DVI_CFG adafruit_dvibell_cfg // PiCowbell DVI;
                                     // pico_sock_cfg // Pico DVI Sock
                                     // pimoroni_demo_hdmi_cfg // Pimoroni Pico DV
#endif

DVIGFX16 dvi(DVI_RES_320x240p60, DVI_CFG);

Adafruit_Monster_Eyes eyes(&dvi, 1);

Eyes_SyncPrimary sync(eyes); // Defaults: Wire, address 0x42, right eye

void setup() {
  Serial.begin(115200);
  eyes.setVerbose(Serial);

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

  sync.begin();
}

void loop() {
  eyes.animate();
  sync.send();

  static uint32_t last = 0;
  if (millis() - last >= 1000) {
    last = millis();
    Serial.printf("%.0f fps\n", eyes.frameRate());
    Serial.printf("sync: sent %lu, failed %lu\n", sync.sent(), sync.failed());
    sync.resetCounts();
  }
}
