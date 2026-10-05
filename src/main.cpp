// ESP32-S2: USB Audio -> I2S DAC. Simpel: tanpa prefill, tanpa drift compensation, tanpa UART/web.
#include "AudioTools.h"
#include "AudioTools/Communication/USB/USBAudioStream.h"
#include <esp_system.h>

#define LED_PIN 15

const int    FIFO_PACKETS = 16;    // 1 paket = 1 ms = 176 byte. Naikkan ke 32-48 kalau suara pecah
const size_t CHUNK = 512;          // kelipatan 4 (frame stereo 16-bit)

AudioInfo info(44100, 2, 16);
USBAudioStream usbIn;
I2SStream i2sOut;

// Kedip LED saat boot: 1=power on, 2=panic, 3=brownout, 4=watchdog, 5=lainnya
// (boleh dihapus kalau sudah tidak perlu)
void blinkReason() {
  esp_reset_reason_t r = esp_reset_reason();
  int b = (r == ESP_RST_POWERON) ? 1 : (r == ESP_RST_PANIC) ? 2 : (r == ESP_RST_BROWNOUT) ? 3 :
          (r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_WDT) ? 4 : 5;
  for (int i = 0; i < b; i++) { digitalWrite(LED_PIN, HIGH); delay(250); digitalWrite(LED_PIN, LOW); delay(250); }
  delay(600);
}

void setup() {
  pinMode(LED_PIN, OUTPUT);
  blinkReason();

  if (!TinyUSBDevice.isInitialized()) TinyUSBDevice.begin(0);

  auto usb_cfg = usbIn.defaultConfig(RX_MODE);
  usb_cfg.copyFrom(info);
  usb_cfg.manufacturer = "ESP32 Audio";
  usb_cfg.product = "ESP32-S2 DAC";
  usb_cfg.serial = "000001";
  usb_cfg.volume_active = false;
  usb_cfg.fifo_packets = FIFO_PACKETS;
  usbIn.begin(usb_cfg);

  auto i2s_cfg = i2sOut.defaultConfig(TX_MODE);
  i2s_cfg.copyFrom(info);
  i2s_cfg.pin_bck = 16; i2s_cfg.pin_data = 17; i2s_cfg.pin_ws = 18;
  i2s_cfg.buffer_count = 8; i2s_cfg.buffer_size = 1024;
  i2sOut.begin(i2s_cfg);

  if (TinyUSBDevice.mounted()) { TinyUSBDevice.detach(); delay(10); TinyUSBDevice.attach(); }
}

void loop() {
  static uint8_t buf[CHUNK];
  static uint32_t lastData = 0;

  size_t n = usbIn.available();
  if (n > CHUNK) n = CHUNK;
  n &= ~3u;                                // jaga alignment frame

  if (n >= 4) {
    n = usbIn.readBytes(buf, n) & ~3u;
    if (n >= 4) {
      i2sOut.write(buf, n);                // blocking -> dipacu clock I2S
      lastData = millis();
    }
  }

  if (n < 4) delay(1);                     // WAJIB: setiap jalur idle harus yield, kalau tidak watchdog reset

  digitalWrite(LED_PIN, (millis() - lastData < 300) ? HIGH : LOW);
}
