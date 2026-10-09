#include "AudioTools.h"
#include "AudioTools/Communication/USB/USBAudioStream.h"

#define LED_PIN 15

AudioInfo info(44100, 2, 16);
USBAudioStream usbIn;
I2SStream i2sOut;

// PERBAIKAN 1: Turunkan ukuran buffer StreamCopy ke 512 byte.
// 512 byte hanya memblokir CPU ~2.9ms, sehingga penerimaan paket USB berjalan lancar.
StreamCopy copier(i2sOut, usbIn, 512);

unsigned long lastAudioTime = 0;
const unsigned long audioTimeout = 300;

unsigned long silenceStart = 0;
bool inSilence = false;
unsigned long underrunCount = 0;

void setup() {
  Serial.begin(115200);
  delay(1000);

  AudioLogger::instance().begin(Serial, AudioLogger::Warning);

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  if (!TinyUSBDevice.isInitialized()) {
    TinyUSBDevice.begin(0);
  }

  auto usb_cfg = usbIn.defaultConfig(RX_MODE);
  usb_cfg.copyFrom(info);

  usb_cfg.manufacturer = "ESP32 Audio";
  usb_cfg.product = "ESP32-S2 DAC";
  usb_cfg.serial = "000001";

  // PERBAIKAN 2: Matikan kalkulasi volume bawaan agar CPU single-core tidak terbeban
  usb_cfg.volume_active = false;

  // PERBAIKAN 3: Sediakan penampung FIFO USB sebesar 64 paket (~64ms)
  usb_cfg.fifo_packets = 64;

  usbIn.begin(usb_cfg);

  auto i2s_cfg = i2sOut.defaultConfig(TX_MODE);
  i2s_cfg.copyFrom(info);

  i2s_cfg.pin_bck  = 16;
  i2s_cfg.pin_data = 17;
  i2s_cfg.pin_ws   = 18;

  // PERBAIKAN 4: Samakan ukuran buffer DMA I2S dengan StreamCopy (512 byte)
  i2s_cfg.buffer_count = 6;
  i2s_cfg.buffer_size  = 512;

  i2sOut.begin(i2s_cfg);

  if (TinyUSBDevice.mounted()) {
    TinyUSBDevice.detach();
    delay(10);
    TinyUSBDevice.attach();
  }

  Serial.println("Setup selesai, USB soundcard siap.");
}

void loop() {
  size_t bytesCopied = copier.copy();
  unsigned long now = millis();

  if (bytesCopied > 0) {
    digitalWrite(LED_PIN, HIGH);
    lastAudioTime = now;

    if (inSilence) {
      unsigned long gapDuration = now - silenceStart;
      underrunCount++;
      Serial.printf("[GAP #%lu] Audio kosong selama %lu ms\n", underrunCount, gapDuration);
      inSilence = false;
    }
  } else {
    if (!inSilence) {
      silenceStart = now;
      inSilence = true;
    }
    if (now - lastAudioTime > audioTimeout) {
      digitalWrite(LED_PIN, LOW);
    }

    // PERBAIKAN 5: Istirahatkan CPU 1ms saat data kosong agar TinyUSB 
    // sempat menyedot data baru dari USB Host tanpa membekukan sistem.
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}
