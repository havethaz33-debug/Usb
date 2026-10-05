#include "AudioTools.h"
#include "AudioTools/Communication/USB/USBAudioStream.h"
#include "esp_system.h"

// ================== KONFIGURASI ==================
// Pin LED bawaan ESP32-S2 Mini (Lolin S2 Mini)
#define LED_PIN 15

// Toleransi jeda (ms) sebelum LED dianggap "musik berhenti".
const unsigned long AUDIO_TIMEOUT_MS = 300;

AudioInfo info(44100, 2, 16);
USBAudioStream usbIn;
I2SStream i2sOut;

// Buffer StreamCopy diperbesar dari default (~1024 byte) ke 4096 byte.
// Kalau error compile, pakai: StreamCopy copier(i2sOut, usbIn);
StreamCopy copier(i2sOut, usbIn, 4096);

// ================== STATUS INTERNAL ==================
unsigned long lastAudioTime = 0;

// MODIF 1: kedipkan LED sesuai penyebab reset terakhir
// 3x = brownout, 4x = panic/crash, 5x = watchdog, 1x = normal
void blinkResetReason() {
  esp_reset_reason_t r = esp_reset_reason();
  int blinks = 1;
  if (r == ESP_RST_BROWNOUT) blinks = 3;
  else if (r == ESP_RST_PANIC) blinks = 4;
  else if (r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT) blinks = 5;

  pinMode(LED_PIN, OUTPUT);
  for (int i = 0; i < blinks; i++) {
    digitalWrite(LED_PIN, HIGH);
    delay(200);
    digitalWrite(LED_PIN, LOW);
    delay(200);
  }
  delay(800);
}

void setup() {
  blinkResetReason();

  Serial.begin(115200);
  delay(1000); // beri waktu USB CDC siap sebelum log pertama tampil

  // Reset reason: 1=power-on, 3=software, 4=panic, 5=int WDT,
  // 6=task WDT, 15=brownout
  Serial.printf("Reset reason: %d\n", (int)esp_reset_reason());

  // Warning-level biar nggak berisik.
  AudioLogger::instance().begin(Serial, AudioLogger::Warning);

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  // Diperlukan di beberapa core supaya TinyUSB benar-benar siap
  // sebelum interface USB Audio dikonfigurasi.
  if (!TinyUSBDevice.isInitialized()) {
    TinyUSBDevice.begin(0);
  }

  // --- Konfigurasi USB Audio Receiver (input dari HP/PC) ---
  auto usb_cfg = usbIn.defaultConfig(RX_MODE);
  usb_cfg.copyFrom(info);

  usb_cfg.manufacturer = "ESP32 Audio";
  usb_cfg.product = "ESP32-S2 DAC";
  usb_cfg.serial = "000001";

  // Tetap dimatikan (dicurigai bikin noise di bagian keras).
  usb_cfg.volume_active = false;

  // Sama seperti kode awal (~48ms).
  usb_cfg.fifo_packets = 48;

  usbIn.begin(usb_cfg);

  // --- Konfigurasi I2S Output ke DAC PCM5100A ---
  auto i2s_cfg = i2sOut.defaultConfig(TX_MODE);
  i2s_cfg.copyFrom(info);

  i2s_cfg.pin_bck  = 16;  // BCK DAC
  i2s_cfg.pin_data = 17;  // DIN DAC
  i2s_cfg.pin_ws   = 18;  // LCK/WS DAC

  // Sama seperti kode awal.
  i2s_cfg.buffer_count = 8;
  i2s_cfg.buffer_size  = 1024;

  i2sOut.begin(i2s_cfg);

  // Paksa host re-enumerate supaya interface audio dikenali.
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
    digitalWrite(LED_PIN, HIGH); // LED nyala kalau ada musik
    lastAudioTime = now;
  } else {
    if (now - lastAudioTime > AUDIO_TIMEOUT_MS) {
      digitalWrite(LED_PIN, LOW); // LED mati kalau musik pause/berhenti
    }
    // MODIF 2: kasih napas ke idle task/watchdog & TinyUSB
    delay(1);
  }
  // MODIF 3: Serial.printf [GAP] di loop dibuang supaya tidak
  // mengganggu timing audio.
}
