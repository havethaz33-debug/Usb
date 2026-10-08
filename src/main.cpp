#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#include "AudioTools.h"
#include "AudioTools/Communication/USB/USBAudioStream.h"
#include <esp_system.h>

SET_LOOP_TASK_STACK_SIZE(16 * 1024);   // stack loop() 16 KB

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

// ---- Catatan crash di RTC memory ----
RTC_NOINIT_ATTR uint32_t rtcMagic;
RTC_NOINIT_ATTR uint32_t rtcStage;
RTC_NOINIT_ATTR uint32_t rtcPlaySecs;
RTC_NOINIT_ATTR uint32_t rtcAv;
RTC_NOINIT_ATTR uint32_t rtcN;
RTC_NOINIT_ATTR uint32_t rtcStkL;
RTC_NOINIT_ATTR uint32_t rtcStkU;
#define MAGIC 0xA5C4
#define STAGE(x) (rtcStage = (x))
const char* const STAGE_NAME[] = {"-", "usb", "i2s", "att", "wait", "fifo", "read", "write", "empty"};

// ---- Statistik untuk OLED ----
volatile uint32_t gLevel = 0, gDrops = 0, gDups = 0, gGapMax = 0, gBadAv = 0;
volatile bool gPlaying = false;
char resetText[24] = "";
char crashDetail[28] = "";
TaskHandle_t loopHandle = nullptr;

static void fmtKB(char* out, size_t sz, uint32_t bytes, bool known = true) {
  if (!known) { snprintf(out, sz, "?"); return; }
  snprintf(out, sz, "%lu.%lu", (unsigned long)(bytes / 1024), (unsigned long)((bytes % 1024) * 10 / 1024));
}

void readResetInfo() {
  esp_reset_reason_t r = esp_reset_reason();
  crashDetail[0] = 0;
  if (r == ESP_RST_PANIC) {
    if (rtcMagic == MAGIC) {
      int st = (rtcStage <= 8) ? (int)rtcStage : 0;
      if (rtcPlaySecs > 0) snprintf(resetText, sizeof(resetText), "PANIC @%s %lus", STAGE_NAME[st], (unsigned long)(rtcPlaySecs - 1));
      else                 snprintf(resetText, sizeof(resetText), "PANIC @%s", STAGE_NAME[st]);
      char l[8], u[8];
      fmtKB(l, sizeof(l), rtcStkL);
      fmtKB(u, sizeof(u), rtcStkU, rtcStkU != 0);
      snprintf(crashDetail, sizeof(crashDetail), "a%lu n%lu L%s U%s", (unsigned long)rtcAv, (unsigned long)rtcN, l, u);
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
  rtcStage = 0; rtcPlaySecs = 0; rtcAv = 0; rtcN = 0; rtcStkL = 0; rtcStkU = 0;
}

void drawScreen() {
  uint32_t stkL = loopHandle ? (uint32_t)uxTaskGetStackHighWaterMark(loopHandle) : 0;
  rtcStkL = stkL;
  rtcStkU = 0; // Matikan pembacaan stack usbd langsung demi stabilitas interrupt

  char sl[8], su[8];
  fmtKB(sl, sizeof(sl), stkL, loopHandle != nullptr);
  fmtKB(su, sizeof(su), 0, false);

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
  oled.setDrawColor(2);                              
  oled.drawVLine(30 + 96 * 25 / 100, 13, 11);        
  oled.drawVLine(30 + 96 * 70 / 100, 13, 11);        
  oled.setDrawColor(1);

  snprintf(line, sizeof(line), "gap %lums heap %uk", (unsigned long)gGapMax, (unsigned)(ESP.getMinFreeHeap() / 1024));
  oled.drawStr(0, 34, line);

  oled.drawStr(0, 46, resetText);

  if (crashDetail[0] && ((millis() / 3000) % 2 == 0)) {
    oled.drawStr(0, 58, crashDetail);
  } else {
    snprintf(line, sizeof(line), "L%s U%s d%lu/%lu b%lu", sl, su, (unsigned long)gDrops, (unsigned long)gDups, (unsigned long)gBadAv);
    oled.drawStr(0, 58, line);
  }
  oled.sendBuffer();                                 
}

void oledTask(void*) {
  for (;;) {
    drawScreen();
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

void setup() {
  pinMode(LED_PIN, OUTPUT);
  loopHandle = xTaskGetCurrentTaskHandle();
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
  i2s_cfg.buffer_count = 6; 
  i2s_cfg.buffer_size = 512; // Disamakan dengan CHUNK (512) agar DMA seragam
  i2sOut.begin(i2s_cfg);

  STAGE(3);
  if (TinyUSBDevice.mounted()) { TinyUSBDevice.detach(); delay(10); TinyUSBDevice.attach(); }
}

void loop() {
  // Tambah alokasi buffer aman (+64 byte) untuk mencegah overflow saat duplicating frame
  static uint8_t buf[CHUNK + 64] __attribute__((aligned(4)));
  static bool playing = false;
  static uint32_t emptySince = 0, lastAdj = 0, playStart = 0;

  STAGE(5);
  size_t av = usbIn.available();
  uint32_t now = millis();
  gLevel = av;

  if (av > 2 * FIFO_BYTES) { gBadAv = gBadAv + 1; delay(1); return; }

  // Prefill buffer sebelum mulai
  if (!playing) {
    if (av >= PREFILL) { playing = true; gPlaying = true; emptySince = 0; playStart = now; }
    else { 
      STAGE(4); 
      rtcPlaySecs = 0; 
      digitalWrite(LED_PIN, LOW); 
      memset(buf, 0, 128);
      i2sOut.write(buf, 128);
      return; 
    }
  }
  rtcPlaySecs = 1 + (now - playStart) / 1000;

  // Bebani I2S dengan data hening jika stream kosong
  if (av == 0) {
    STAGE(8);
    if (!emptySince) emptySince = now;
    if (now - emptySince > AUDIO_TIMEOUT_MS) { playing = false; gPlaying = false; rtcPlaySecs = 0; }
    
    memset(buf, 0, CHUNK);
    i2sOut.write(buf, CHUNK);
    return;
  }

  if (emptySince) {
    uint32_t gap = now - emptySince;
    if (gap > gGapMax) gGapMax = gap;
    emptySince = 0;
  }

  // Drift compensation: Buang sampel langsung dari USB FIFO (bukan memotong panjang `w`)
  if (now - lastAdj >= 20) {
    if (av > HIGH_MARK) {
      uint8_t discardBuf[4];
      usbIn.readBytes(discardBuf, 4); // Buang 1 frame stereo langsung dari USB ringbuffer
      av -= 4;
      lastAdj = now;
      gDrops++;
    }
  }

  size_t n = (av < CHUNK ? av : CHUNK) & ~3u;
  if (n < 4) { 
    memset(buf, 0, 4);
    i2sOut.write(buf, 4);
    return; 
  }

  STAGE(6);
  rtcAv = av;
  rtcN = n;
  n = usbIn.readBytes(buf, n) & ~3u;
  if (n < 4) { 
    memset(buf, 0, 4);
    i2sOut.write(buf, 4);
    return; 
  }

  size_t w = n;
  // Penggandaan frame saat buffer menipis
  if (now - lastAdj >= 20 && av < LOW_MARK && n >= 4) {
    memcpy(buf + n, buf + n - 4, 4);
    w = n + 4;
    lastAdj = now;
    gDups++;
  }

  STAGE(7);
  i2sOut.write(buf, w);
  digitalWrite(LED_PIN, HIGH);
}
