#include <Arduino.h>
#include "AudioTools.h"
#include "AudioTools/Communication/USB/USBAudioStream.h"

SET_LOOP_TASK_STACK_SIZE(16 * 1024);   // Stack loop() 16 KB

#define LED_PIN 15

const uint32_t AUDIO_TIMEOUT_MS = 300;
const int      FIFO_PACKETS     = 48;                 
const size_t   FIFO_BYTES       = FIFO_PACKETS * 176;
const size_t   PREFILL          = FIFO_BYTES * 50 / 100;
const size_t   HIGH_MARK        = FIFO_BYTES * 70 / 100;
const size_t   LOW_MARK         = FIFO_BYTES * 25 / 100;
const size_t   CHUNK            = 512;

AudioInfo info(44100, 2, 16);
USBAudioStream usbIn;
I2SStream i2sOut;

void setup() {
  pinMode(LED_PIN, OUTPUT);

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
  i2s_cfg.buffer_count = 8; 
  i2s_cfg.buffer_size = 512; 
  i2sOut.begin(i2s_cfg);

  if (TinyUSBDevice.mounted()) { 
    TinyUSBDevice.detach(); 
    delay(10); 
    TinyUSBDevice.attach(); 
  }
}

void loop() {
  static uint8_t buf[CHUNK + 64] __attribute__((aligned(4)));
  static bool playing = false;
  static uint32_t emptySince = 0, lastAdj = 0;

  size_t av = usbIn.available();
  uint32_t now = millis();

  // Jika pembacaan available ngawur
  if (av > 2 * FIFO_BYTES) { delay(1); return; }

  // Prefill buffer sebelum mulai pemutaran
  if (!playing) {
    if (av >= PREFILL) { 
      playing = true; 
      emptySince = 0; 
    } else { 
      digitalWrite(LED_PIN, LOW); 
      memset(buf, 0, CHUNK);
      i2sOut.write(buf, CHUNK);
      return; 
    }
  }

  // Jika stream USB kosong, suplai data hening (silence) ke I2S
  if (av == 0) {
    if (!emptySince) emptySince = now;
    if (now - emptySince > AUDIO_TIMEOUT_MS) { playing = false; }
    
    memset(buf, 0, CHUNK);
    i2sOut.write(buf, CHUNK);
    return;
  }

  if (emptySince) emptySince = 0;

  // Drift compensation
  size_t bytesToRead = CHUNK;
  bool doDrop = false, doDup = false;

  if (now - lastAdj >= 50) {
    if (av > HIGH_MARK) {
      bytesToRead = CHUNK + 4;
      doDrop = true;
    } else if (av < LOW_MARK && av >= CHUNK) {
      bytesToRead = CHUNK - 4;
      doDup = true;
    }
  }

  if (bytesToRead > av) {
    bytesToRead = av & ~3u;
    doDrop = false;
    doDup = false;
  }

  if (bytesToRead < 4) {
    memset(buf, 0, CHUNK);
    i2sOut.write(buf, CHUNK);
    return;
  }

  size_t bytesRead = usbIn.readBytes(buf, bytesToRead) & ~3u;

  if (bytesRead < 4) {
    memset(buf, 0, CHUNK);
    i2sOut.write(buf, CHUNK);
    return;
  }

  // Ratakan ukuran buffer penulisan agar selalu tepat 512 byte
  if (doDrop && bytesRead == CHUNK + 4) {
    lastAdj = now;
  } else if (doDup && bytesRead == CHUNK - 4) {
    memcpy(buf + CHUNK - 4, buf + CHUNK - 8, 4);
    lastAdj = now;
  } else if (bytesRead < CHUNK) {
    memset(buf + bytesRead, 0, CHUNK - bytesRead);
  }

  // Kirim ke DMA I2S
  i2sOut.write(buf, CHUNK);
  digitalWrite(LED_PIN, HIGH);
}
