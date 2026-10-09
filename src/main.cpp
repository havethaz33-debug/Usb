#define USE_TINYUSB 1
#include "USB.h"
#include "AudioTools.h"

#define LED_PIN 15
#define TX_PIN  40  // Pin TX telemetri ke ESP32-C3

AudioInfo info(44100, 2, 16);
USBAudioStream usbIn;
I2SStream i2sOut;

unsigned long lastAudioTime = 0;
unsigned long lastTelemetryTime = 0;
const unsigned long audioTimeout = 300;

unsigned long underrunCount = 0;
uint8_t audioPeak = 0;
bool isPlaying = false;

uint8_t audioBuf[512];

void setup() {
  Serial1.begin(115200, SERIAL_8N1, -1, TX_PIN);

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  if (!TinyUSBDevice.isInitialized()) {
    TinyUSBDevice.begin(0);
  }

  auto usb_cfg = usbIn.defaultConfig(RX_MODE);
  usb_cfg.copyFrom(info);
  usb_cfg.manufacturer  = "ESP32 Audio";
  usb_cfg.product       = "ESP32-S2 DAC";
  usb_cfg.serial        = "000001";
  usb_cfg.volume_active = false;
  usb_cfg.fifo_packets  = 64;
  usbIn.begin(usb_cfg);

  auto i2s_cfg = i2sOut.defaultConfig(TX_MODE);
  i2s_cfg.copyFrom(info);
  i2s_cfg.pin_bck     = 16;
  i2s_cfg.pin_data    = 17;
  i2s_cfg.pin_ws      = 18;
  i2s_cfg.buffer_count = 6;
  i2s_cfg.buffer_size  = 512;
  i2sOut.begin(i2s_cfg);

  if (TinyUSBDevice.mounted()) {
    TinyUSBDevice.detach();
    delay(10);
    TinyUSBDevice.attach();
  }
}

void loop() {
  size_t avail = usbIn.available();
  unsigned long now = millis();

  // Kuras penumpukan buffer saat ganti lagu
  if (avail > 2048) {
    while (usbIn.available() > 512) {
      usbIn.readBytes(audioBuf, 512);
    }
    avail = usbIn.available();
  }

  if (avail >= 512) {
    size_t readBytes = usbIn.readBytes(audioBuf, 512);
    if (readBytes > 0) {
      i2sOut.write(audioBuf, readBytes);
      digitalWrite(LED_PIN, HIGH);
      lastAudioTime = now;
      isPlaying = true;

      int32_t maxVal = 0;
      int16_t *samples = (int16_t *)audioBuf;
      for (size_t i = 0; i < readBytes / 2; i += 8) {
        int16_t val = abs(samples[i]);
        if (val > maxVal) maxVal = val;
      }
      audioPeak = map(constrain(maxVal, 0, 32767), 0, 32767, 0, 100);
    }
  } else if (avail == 0) {
    if (isPlaying && (now - lastAudioTime > audioTimeout)) {
      digitalWrite(LED_PIN, LOW);
      isPlaying = false;
      audioPeak = 0;
      underrunCount++;
    }
    vTaskDelay(pdMS_TO_TICKS(1));
  }

  // Kirim telemetri 5 variabel ke ESP32-C3 tiap 150ms
  if (now - lastTelemetryTime >= 150) {
    lastTelemetryTime = now;

    uint32_t freeHeapKb = ESP.getFreeHeap() / 1024;
    uint8_t statusVal = isPlaying ? 1 : 0;

    Serial1.printf("DATA:%d,%d,%lu,%lu,%d\n", 
                   statusVal, 
                   info.sample_rate, 
                   freeHeapKb, 
                   underrunCount, 
                   audioPeak);
  }
}
