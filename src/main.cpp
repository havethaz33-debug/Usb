// ESP32-S2 (LOLIN S2 mini): USB Audio -> I2S DAC + layar status OLED (U8g2, I2C).
// Audio jalan di loop(). OLED digambar di task terpisah supaya I2C yang lambat tidak mengganggu audio.
//
// Pin:  I2S BCK=16 DATA=17 WS=18 | OLED SDA=33 SCL=35 (default S2 mini) | LED=15
// OLED: SSD1306 128x64 I2C (alamat 0x3C). Kalau layarnya SH1106 (1.3"), ganti kelas "oled" di bawah.
//
// Layar menampilkan:
//   baris 1: PLAY/IDLE + uptime (kalau uptime ke-reset = ada restart)
//   baris 2: isi FIFO USB (garis = batas LOW 25% dan HIGH 70% untuk drift compensation)
//   baris 3: drop/dup = berapa kali drift compensation membuang/menggandakan 1 frame
//   baris 4: gap = FIFO kosong terlama (ms), heap = free heap terendah (KB; turun terus = memory leak)
//   baris 5: penyebab restart terakhir, mis. "PANIC @write 45s" = crash saat nulis I2S, 45 dtk setelah mulai main
#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#include "AudioTools.h"
#include "AudioTools/Communication/USB/USBAudioStream.h"
#include <esp_system.h>

#define LED_PIN  15
#define OLED_SDA 33
#define OLED_SCL 35

const uint32_t AUDIO_TIMEOUT_MS = 300;
const int      FIFO_PACKETS = 48;                 // 1 paket = 1 ms = 176 byte
const size_t   FIFO_BYTES = FIFO_PACKETS * 176;
const size_t   PREFILL    = FIFO_BYTES * 50 / 100;
const size_t   HIGH_MARK  = FIFO_BYTES * 70 / 100;
const size_t   LOW_MARK   = FIFO_BYTES * 25 / 100;
const size_t   CHUNK = 512;

AudioInfo info(44100, 2, 16);
USBAudioStream usbIn;
I2SStream i2sOut;

U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE);
// U8G2_SH1106_128X64_NONAME_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE);   // <- untuk layar SH1106

// ---- Catatan crash di RTC memory (bertahan setelah panic-reboot) ----
RTC_NOINIT_ATTR uint32_t rtcMagic;
RTC_NOINIT_ATTR uint32_t rtcStage;
RTC_NOINIT_ATTR uint32_t rtcPlaySecs;
#define MAGIC 0xA5C3
#define STAGE(x) (rtcStage = (x))
const char* const STAGE_NAME[] = {"-", "usb", "i2s", "att", "wait", "fifo", "read", "write", "empty"};

// ---- Statistik untuk OLED (ditulis loop(), dibaca task OLED) ----
volatile uint32_t gLevel = 0, gDrops = 0, gDups = 0, gGapMax = 0;
volatile bool gPlaying = false;
char resetText[24] = "";

void readResetInfo() {
  esp_reset_reason_t r = esp_reset_reason();
  if (r == ESP_RST_PANIC) {
    if (rtcMagic == MAGIC) {
      int st = (rtcStage <= 8) ? (int)rtcStage : 0;
      if (rtcPlaySecs > 0) snprintf(resetText, sizeof(resetText), "PANIC @%s %lus", STAGE_NAME[st], (unsigned long)(rtcPlaySecs - 1));
      else                 snprintf(resetText, sizeof(resetText), "PANIC @%s", STAGE_NAME[st]);
    } else {
      snprintf(resetText, sizeof(resetText), "PANIC (?)");
    }
  } else if (r == ESP_RST_POWERON) {
    snprintf(resetText, sizeof(resetText), "POWER ON");
  } else if (r == ESP_RST_BROWNOUT) {
    snprintf(resetText, sizeof(resetText), "BROWNOUT");
  } else if (r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_WDT) {
    snprintf(resetText, sizeof(resetText), "WATCHDOG");
  } else {
    snprintf(resetText, sizeof(resetText), "RESET (%d)", (int)r);
  }
  rtcMagic = MAGIC;
  rtcStage = 0;
  rtcPlaySecs = 0;
}

void drawScreen() {
  char line[32];
  int pct = (int)(((uint64_t)gLevel * 100) / FIFO_BYTES);
  if (pct > 100) pct = 100;

  oled.clearBuffer();
  oled.setFont(u8g2_font_6x10_tf);

  snprintf(line, sizeof(line), "USB DAC %s %lus", gPlaying ? "PLAY" : "IDLE", (unsigned long)(millis() / 1000));
  oled.drawStr(0, 10, line);

  oled.drawStr(0, 22, "FIFO");
  oled.drawFrame(28, 14, 100, 9);
  oled.drawBox(30, 16, pct * 96 / 100, 5);
  oled.setDrawColor(2);                              // XOR supaya penanda terlihat di atas isi bar
  oled.drawVLine(30 + 96 * 25 / 100, 13, 11);        // LOW
  oled.drawVLine(30 + 96 * 70 / 100, 13, 11);        // HIGH
  oled.setDrawColor(1);

  snprintf(line, sizeof(line), "drop %lu dup %lu", (unsigned long)gDrops, (unsigned long)gDups);
  oled.drawStr(0, 34, line);

  snprintf(line, sizeof(line), "gap %lums heap %uk", (unsigned long)gGapMax, (unsigned)(ESP.getMinFreeHeap() / 1024));
  oled.drawStr(0, 46, line);

  oled.drawStr(0, 58, resetText);
  oled.sendBuffer();                                 // ~25 ms di I2C 400 kHz, aman karena di task sendiri
}

void oledTask(void*) {
  for (;;) {
    drawScreen();
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

void setup() {
  pinMode(LED_PIN, OUTPUT);
  readResetInfo();

  Wire.begin(OLED_SDA, OLED_SCL);
  oled.setBusClock(400000);
  oled.begin();
  xTaskCreate(oledTask, "oled", 4096, nullptr, 1, nullptr);

  STAGE(1);
  if (!TinyUSBDevice.isInitialized()) TinyUSBDevice.begin(0);
  auto usb_cfg = usbIn.defaultConfig(RX_MODE);
  usb_cfg.copyFrom(info);
  usb_cfg.manufacturer = "ESP32 Audio";
  usb_cfg.product = "ESP32-S2 DAC";
  usb_cfg.serial = "000001";
  usb_cfg.volume_active = false;
  usb_cfg.fifo_packets = FIFO_PACKETS;
  usbIn.begin(usb_cfg);

  STAGE(2);
  auto i2s_cfg = i2sOut.defaultConfig(TX_MODE);
  i2s_cfg.copyFrom(info);
  i2s_cfg.pin_bck = 16; i2s_cfg.pin_data = 17; i2s_cfg.pin_ws = 18;
  i2s_cfg.buffer_count = 8; i2s_cfg.buffer_size = 1024;
  i2sOut.begin(i2s_cfg);

  STAGE(3);
  if (TinyUSBDevice.mounted()) { TinyUSBDevice.detach(); delay(10); TinyUSBDevice.attach(); }
}

void loop() {
  static uint8_t buf[CHUNK + 4];
  static bool playing = false;
  static uint32_t emptySince = 0, lastAdj = 0, playStart = 0;

  STAGE(5);
  size_t av = usbIn.available();
  uint32_t now = millis();
  gLevel = av;

  // Isi FIFO dulu sebelum mulai main, supaya tahan jitter
  if (!playing) {
    if (av >= PREFILL) { playing = true; gPlaying = true; emptySince = 0; playStart = now; }
    else { STAGE(4); rtcPlaySecs = 0; digitalWrite(LED_PIN, LOW); delay(1); return; }
  }
  rtcPlaySecs = 1 + (now - playStart) / 1000;

  // FIFO kosong: kalau lebih dari AUDIO_TIMEOUT_MS anggap stream berhenti, isi ulang dulu
  if (av == 0) {
    STAGE(8);
    if (!emptySince) emptySince = now;
    if (now - emptySince > AUDIO_TIMEOUT_MS) { playing = false; gPlaying = false; rtcPlaySecs = 0; }
    delay(1);
    return;
  }
  if (emptySince) {
    uint32_t gap = now - emptySince;
    if (gap > gGapMax) gGapMax = gap;
    emptySince = 0;
  }

  size_t n = (av < CHUNK ? av : CHUNK) & ~3u;   // jaga alignment frame stereo 16-bit
  if (n < 4) { delay(1); return; }

  STAGE(6);
  n = usbIn.readBytes(buf, n) & ~3u;
  if (n < 4) { delay(1); return; }              // setiap jalur harus yield

  // Drift compensation: samakan laju USB dengan clock I2S
  size_t w = n;
  if (now - lastAdj >= 20) {
    if (av > HIGH_MARK && n >= 8) {             // FIFO menumpuk -> buang 1 frame
      w = n - 4; lastAdj = now; gDrops = gDrops + 1;
    } else if (av < LOW_MARK) {                 // FIFO menipis -> gandakan 1 frame
      memcpy(buf + n, buf + n - 4, 4);
      w = n + 4; lastAdj = now; gDups = gDups + 1;
    }
  }

  STAGE(7);
  i2sOut.write(buf, w);                         // blocking = dipacu clock I2S
  digitalWrite(LED_PIN, HIGH);
}
