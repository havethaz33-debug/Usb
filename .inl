#include "AudioTools.h"
#include "AudioTools/Communication/USB/USBAudioStream.h"
#include <WiFi.h>
#include <WiFiUdp.h>

const char* AP_SSID = "ESP-AUDIO";
const char* AP_PASS = "audio12345";   // minimal 8 karakter
const uint16_t PORT = 4210;

const int PAYLOAD = 1024;             // 256 frame stereo 16-bit (~5,8 ms)

USBAudioStream usb;
WiFiUDP udp;

IPAddress remoteIP;
bool haveRemote = false;

uint8_t pkt[2 + PAYLOAD];
size_t filled = 0;
uint16_t seq = 0;

void setup() {
  Serial.begin(115200);

  // WiFi AP
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS, 1, 0, 2);
  WiFi.setSleep(false);               // penting: matikan power save
  udp.begin(PORT);

  // USB audio (RX = terima dari HP)
  auto cfg = usb.defaultConfig(RX_MODE);
  cfg.sample_rate = 44100;
  cfg.channels = 2;
  cfg.bits_per_sample = 16;
  usb.begin(cfg);

  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());
}

void loop() {
  // Terima "hello" dari C3 untuk tahu alamat IP-nya
  int n = udp.parsePacket();
  if (n > 0) {
    remoteIP = udp.remoteIP();
    haveRemote = true;
    uint8_t tmp[16];
    udp.read(tmp, sizeof(tmp));
  }

  // Ambil data dari USB, kumpulkan sampai 1 paket penuh
  int av = usb.available();
  if (av > 0) {
    size_t want = PAYLOAD - filled;
    if ((size_t)av < want) want = av;
    size_t r = usb.readBytes(pkt + 2 + filled, want);
    filled += r;
  } else {
    delay(1);
  }

  if (filled >= PAYLOAD) {
    pkt[0] = seq >> 8;
    pkt[1] = seq & 0xFF;
    seq++;
    if (haveRemote) {
      udp.beginPacket(remoteIP, PORT);
      udp.write(pkt, sizeof(pkt));
      udp.endPacket();
    }
    filled = 0;
  }
}
