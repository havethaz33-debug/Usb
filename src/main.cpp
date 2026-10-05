// S2 UTAMA: pre-fill + drift compensation. Tanpa WiFi.
#include "AudioTools.h"
#include "AudioTools/Communication/USB/USBAudioStream.h"
#include <esp_system.h>

// ===== S2 (audio) -> C3 (web) lewat UART =====
// Kabel: S2 GPIO21 (TX) -> C3 GPIO4 (RX), GND ke GND. Pin 3V3 JANGAN disambung.
#define LED_PIN 15
#define UART_TX 21
#define UART_RX_DUMMY 5   // tidak disambung; supaya Serial1 tidak memakai pin I2S (17/18)
#define ENABLE_DRIFT 1

const uint32_t AUDIO_TIMEOUT_MS = 300;
const int      FIFO_PACKETS = 48;
const size_t   FIFO_BYTES = FIFO_PACKETS * 176;
const size_t   PREFILL   = FIFO_BYTES * 50 / 100;
const size_t   HIGH_MARK = FIFO_BYTES * 70 / 100;
const size_t   LOW_MARK  = FIFO_BYTES * 25 / 100;
const size_t   CHUNK = 512;

AudioInfo info(44100, 2, 16);
USBAudioStream usbIn;
I2SStream i2sOut;

struct Stats {
  volatile bool active = false;
  volatile uint32_t level = 0, maxLevel = 0;
  volatile uint64_t bytes = 0;
  volatile uint32_t underruns = 0, lastGap = 0, maxGap = 0, drops = 0, dups = 0;
} S;

void sendStats() {
  char l[200];
  snprintf(l, sizeof(l), "S,%d,%u,%u,%llu,%u,%u,%u,%u,%u,%u,%lu,%d\n",
    S.active ? 1 : 0, (unsigned)S.level, (unsigned)FIFO_BYTES, (unsigned long long)S.bytes,
    (unsigned)S.underruns, (unsigned)S.lastGap, (unsigned)S.maxGap, (unsigned)S.drops,
    (unsigned)S.dups, (unsigned)ESP.getFreeHeap(), (unsigned long)(millis() / 1000),
    (int)esp_reset_reason());
  Serial1.print(l);
}

// Kedip LED saat boot: 1=power on, 2=panic, 3=brownout, 4=watchdog, 5=lainnya
void blinkReason() {
  esp_reset_reason_t r = esp_reset_reason();
  int b = (r == ESP_RST_POWERON) ? 1 : (r == ESP_RST_PANIC) ? 2 : (r == ESP_RST_BROWNOUT) ? 3 :
          (r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_WDT) ? 4 : 5;
  for (int i = 0; i < b; i++) { digitalWrite(LED_PIN, HIGH); delay(250); digitalWrite(LED_PIN, LOW); delay(250); }
  delay(600);
}

void setupHW() {
  pinMode(LED_PIN, OUTPUT);
  blinkReason();
  Serial1.begin(115200, SERIAL_8N1, UART_RX_DUMMY, UART_TX);
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

void audioStep() {
  static uint8_t buf[CHUNK + 4];
  static bool playing = false;
  static uint32_t emptySince = 0, lastAdj = 0, lastData = 0;

  {
    size_t av = usbIn.available();
    uint32_t now = millis();
    S.level = av;
    if (av > S.maxLevel) S.maxLevel = av;
    if (av) lastData = now;

    // --- Fase menunggu: isi buffer dulu biar tahan jitter ---
    if (!playing) {
      if (av >= PREFILL) {
        playing = true;
        emptySince = 0;
      } else {
        if (now - lastData > AUDIO_TIMEOUT_MS) S.active = false;
        delay(1);
        return;
      }
    }

    // --- Buffer kosong saat play = underrun ---
    if (av == 0) {
      if (!emptySince) emptySince = now;
      if (now - emptySince > AUDIO_TIMEOUT_MS) {
        playing = false;
        S.active = false;
      }
      delay(1);
      return;
    }
    if (emptySince) {
      uint32_t gap = now - emptySince;
      S.underruns++;
      S.lastGap = gap;
      if (gap > S.maxGap) S.maxGap = gap;
      emptySince = 0;
    }

    size_t n = (av < CHUNK ? av : CHUNK) & ~3u;   // jaga alignment frame stereo 16-bit
    if (n < 4) { delay(1); return; }
    n = usbIn.readBytes(buf, n) & ~3u;
    if (n < 4) return;

    // --- Drift compensation: samakan laju USB dengan clock I2S ---
    size_t w = n;
    if (ENABLE_DRIFT && now - lastAdj >= 20) {
      if (av > HIGH_MARK && n >= 8) {          // buffer menumpuk -> buang 1 frame
        w = n - 4; S.drops++; lastAdj = now;
      } else if (av < LOW_MARK) {              // buffer menipis -> gandakan 1 frame
        memcpy(buf + n, buf + n - 4, 4);
        w = n + 4; S.dups++; lastAdj = now;
      }
    }

    i2sOut.write(buf, w);   // blocking = dipacu clock I2S
    S.bytes += n;
    S.active = true;
  }
}

void setup() { setupHW(); }

void loop() {
  audioStep();
  static uint32_t t = 0;
  if (millis() - t >= 500) { t = millis(); sendStats(); }
  digitalWrite(LED_PIN, S.active ? HIGH : LOW);
}
