#include "AudioTools.h"
#include "AudioTools/Communication/USB/USBAudioStream.h"

// ================== KONFIGURASI ==================
// Pin LED bawaan ESP32-S2 Mini (Lolin S2 Mini)
#define LED_PIN 15

// Toleransi jeda (ms) sebelum LED dianggap "musik berhenti".
const unsigned long AUDIO_TIMEOUT_MS = 300;

AudioInfo info(44100, 2, 16);
USBAudioStream usbIn;
I2SStream i2sOut;

// Buffer StreamCopy diperbesar dari default (~1024 byte) ke 4096 byte,
// biar lebih tahan jitter USB saat throughput tinggi (mis. FLAC).
// Kalau baris ini error compile di versi library kamu, hapus parameter
// ke-3 dan pakai: StreamCopy copier(i2sOut, usbIn);
StreamCopy copier(i2sOut, usbIn, 4096);

// ================== STATUS INTERNAL ==================
unsigned long lastAudioTime = 0;
unsigned long silenceStart = 0;
bool inSilence = false;
unsigned long underrunCount = 0;

void setup() {
  Serial.begin(115200);
  delay(1000); // beri waktu USB CDC siap sebelum log pertama tampil

  // Warning-level biar nggak berisik pas pemakaian sehari-hari (nggak ada
  // Serial Monitor yang connect). Ganti ke AudioLogger::Info sementara
  // kalau lagi aktif debug & Serial Monitor terbuka.
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

  // Identitas USB, biar Windows/HP kenalin nama device-nya sendiri
  // (bukan generik "USB Audio"). Ganti bebas sesuai selera.
  usb_cfg.manufacturer = "ESP32 Audio";
  usb_cfg.product = "ESP32-S2 DAC";
  usb_cfg.serial = "000001";

  // Slider volume Windows/HP beneran ngefek ke sinyal audio (bukan cuma
  // diterima tapi diabaikan).
  // SEMENTARA DIMATIIN LAGI — dicurigai proses scaling ini yang nyebabin
  // noise/kresek khusus di bagian audio yang keras (vokal). Tes isolasi
  // sebelumnya kemungkinan nggak valid karena versi false-nya belum
  // sempat ke-upload ulang.
  usb_cfg.volume_active = false;

  // Perbesar buffer sisi USB (default 16 paket @1ms = ~16ms) jadi ~48ms.
  // Ini yang paling efektif ngurangin frekuensi buffer/pause, dari
  // sering banget jadi cuma sesekali. Trade-off: latensi nambah dikit.
  usb_cfg.fifo_packets = 48;

  usbIn.begin(usb_cfg);

  // --- Konfigurasi I2S Output ke DAC PCM5100A ---
  auto i2s_cfg = i2sOut.defaultConfig(TX_MODE);
  i2s_cfg.copyFrom(info);

  i2s_cfg.pin_bck  = 16;  // BCK DAC
  i2s_cfg.pin_data = 17;  // DIN DAC
  i2s_cfg.pin_ws   = 18;  // LCK/WS DAC

  // Perbesar buffer DMA I2S supaya lebih tahan telat sesaat dari sisi USB.
  // Kalau baris ini error compile, cek nama field di I2SConfigESP32 versi
  // library kamu (bisa beda dikit antar versi) atau hapus dua baris ini.
  i2s_cfg.buffer_count = 8;
  i2s_cfg.buffer_size  = 1024;

  i2sOut.begin(i2s_cfg);

  // PENTING: paksa host re-enumerate supaya interface audio yang baru
  // didaftarkan (usbIn.begin di atas) benar-benar dikenali host.
  // Tanpa ini host bisa gagal mendeteksi soundcard-nya sama sekali.
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

    // Kalau sebelumnya lagi kosong, catat berapa lama gap-nya.
    // Di-guard pakai availableForWrite supaya Serial.printf ini TIDAK
    // ngeblock loop() kalau nggak ada Serial Monitor yang lagi connect
    // (mis. pas dengerin musik biasa dari HP, bukan sambil debug).
    if (inSilence) {
      unsigned long gapDuration = now - silenceStart;
      underrunCount++;
      if (Serial && Serial.availableForWrite() > 64) {
        Serial.printf("[GAP #%lu] Audio kosong selama %lu ms\n", underrunCount, gapDuration);
      }
      inSilence = false;
    }

    lastAudioTime = now;
  } else {
    if (!inSilence) {
      silenceStart = now;
      inSilence = true;
    }
    if (now - lastAudioTime > AUDIO_TIMEOUT_MS) {
      digitalWrite(LED_PIN, LOW); // LED mati kalau musik pause/berhenti
    }
  }
}
