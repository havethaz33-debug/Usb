#include <Arduino.h>
#include "AudioTools.h"
#include "AudioTools/Communication/USB/USBAudioStream.h"

// Alokasi stack loopTask utama
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

#define LED_PIN 15

const size_t CHUNK_SIZE = 256; // Ukuran buffer DMA kecil agar responsif
AudioInfo info(44100, 2, 16);

USBAudioStream usbIn;
I2SStream i2sOut;

// Pointer buffer disimpan permanen di Heap RAM (bukan Stack)
uint8_t* audioBuffer = nullptr;

void setup() {
  pinMode(LED_PIN, OUTPUT);

  // 1. Alokasi buffer audio di Heap Memory
  audioBuffer = (uint8_t*) malloc(CHUNK_SIZE);
  if (audioBuffer) {
    memset(audioBuffer, 0, CHUNK_SIZE);
  }

  // 2. Inisialisasi USB Input
  if (!TinyUSBDevice.isInitialized()) TinyUSBDevice.begin(0);
  auto usb_cfg = usbIn.defaultConfig(RX_MODE);
  usb_cfg.copyFrom(info);
  usb_cfg.manufacturer = "ESP32 Audio";
  usb_cfg.product = "ESP32-S2 DAC";
  usb_cfg.fifo_packets = 32; // Batasi ukuran FIFO agar beban interrupt USB ringan
  usbIn.begin(usb_cfg);

  // 3. Inisialisasi I2S Output
  auto i2s_cfg = i2sOut.defaultConfig(TX_MODE);
  i2s_cfg.copyFrom(info);
  i2s_cfg.pin_bck = 16;
  i2s_cfg.pin_data = 17;
  i2s_cfg.pin_ws = 18;
  i2s_cfg.buffer_count = 8;
  i2s_cfg.buffer_size = CHUNK_SIZE;
  i2sOut.begin(i2s_cfg);

  // Reset koneksi USB
  if (TinyUSBDevice.mounted()) {
    TinyUSBDevice.detach();
    delay(10);
    TinyUSBDevice.attach();
  }
}

void loop() {
  if (!audioBuffer) return;

  size_t avail = usbIn.available();

  // Jika data USB masuk minimal 1 chunk (256 byte)
  if (avail >= CHUNK_SIZE) {
    size_t toRead = (avail > CHUNK_SIZE) ? CHUNK_SIZE : avail;
    toRead &= ~3u; // Align 4-byte untuk frame stereo 16-bit

    size_t readBytes = usbIn.readBytes(audioBuffer, toRead);
    if (readBytes > 0) {
      i2sOut.write(audioBuffer, readBytes);
      digitalWrite(LED_PIN, HIGH);
    }
  } else {
    // Jika data USB belum siap, kirim silence ke I2S DMA
    memset(audioBuffer, 0, CHUNK_SIZE);
    i2sOut.write(audioBuffer, CHUNK_SIZE);
    digitalWrite(LED_PIN, LOW);

    // KUNCI UTAMA: Wajib vTaskDelay agar Task Watchdog ESP32-S2 single-core tidak Panic
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}
