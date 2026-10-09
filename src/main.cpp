#include <Arduino.h>
#include "AudioTools.h"
#include "AudioTools/Communication/USB/USBAudioStream.h"

// Perluas stack size untuk task loop
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

#define LED_PIN 15

AudioInfo info(44100, 2, 16);
USBAudioStream usbIn;
I2SStream i2sOut;
StreamCopy copier(i2sOut, usbIn); // Alirkan data otomatis dari USB ke I2S

void setup() {
  pinMode(LED_PIN, OUTPUT);

  // Inisialisasi USB Audio Input
  if (!TinyUSBDevice.isInitialized()) TinyUSBDevice.begin(0);
  auto usb_cfg = usbIn.defaultConfig(RX_MODE);
  usb_cfg.copyFrom(info);
  usb_cfg.manufacturer = "ESP32 Audio";
  usb_cfg.product = "ESP32-S2 DAC";
  usb_cfg.serial = "000001";
  usb_cfg.volume_active = false;
  usb_cfg.fifo_packets = 64; // Ukuran FIFO USB yang stabil
  usbIn.begin(usb_cfg);

  // Inisialisasi I2S Output
  auto i2s_cfg = i2sOut.defaultConfig(TX_MODE);
  i2s_cfg.copyFrom(info);
  i2s_cfg.pin_bck = 16;
  i2s_cfg.pin_data = 17;
  i2s_cfg.pin_ws = 18;
  i2s_cfg.buffer_count = 8;
  i2s_cfg.buffer_size = 512;
  i2sOut.begin(i2s_cfg);

  // Refresh koneksi USB
  if (TinyUSBDevice.mounted()) {
    TinyUSBDevice.detach();
    delay(10);
    TinyUSBDevice.attach();
  }
}

void loop() {
  // StreamCopy menangani proses read/write secara otomatis dan aman
  if (copier.copy() > 0) {
    digitalWrite(LED_PIN, HIGH);
  } else {
    digitalWrite(LED_PIN, LOW);
    delay(1); // Istirahatkan CPU saat idle agar Watchdog nggak memicu panic
  }
}
