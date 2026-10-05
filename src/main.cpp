#include "AudioTools.h"
#include "AudioTools/Communication/USB/USBAudioStream.h"
#include <WiFi.h>
#include <WebServer.h>

// ================== KONFIGURASI ==================
// Saklar tes isolasi: nyalakan satu-satu untuk cari penyebab reset.
#define ENABLE_WEB   1   // 0 = tanpa WiFi/web (tes brownout)
#define ENABLE_DRIFT 1   // 0 = tanpa drift compensation
#define LED_PIN 15                       // LED bawaan Lolin S2 Mini
const char* AP_SSID = "ESP32-DAC";       // WiFi monitor (buka 192.168.4.1)
const char* AP_PASS = "12345678";        // min. 8 karakter

const uint32_t AUDIO_TIMEOUT_MS = 300;   // jeda sebelum dianggap "berhenti"
const int      FIFO_PACKETS     = 48;    // buffer USB (~1ms/paket)
const size_t   FIFO_BYTES       = FIFO_PACKETS * 176; // 44.1k*2ch*2B/1000
const size_t   PREFILL  = FIFO_BYTES * 50 / 100;      // isi awal sebelum play
const size_t   HIGH_MARK = FIFO_BYTES * 70 / 100;     // di atas ini: buang 1 frame
const size_t   LOW_MARK  = FIFO_BYTES * 25 / 100;     // di bawah ini: gandakan 1 frame
const size_t   CHUNK = 512;              // byte per siklus (kelipatan 4)

AudioInfo info(44100, 2, 16);
USBAudioStream usbIn;
I2SStream i2sOut;
WebServer server(80);

// ================== STATISTIK (dibaca web) ==================
struct Stats {
  volatile bool     active = false;
  volatile uint32_t level = 0, maxLevel = 0;
  volatile uint64_t bytes = 0;
  volatile uint32_t underruns = 0, lastGap = 0, maxGap = 0;
  volatile uint32_t drops = 0, dups = 0;
} S;

// ================== TASK AUDIO ==================
void audioTask(void*) {
  static uint8_t buf[CHUNK + 4];
  bool playing = false;
  uint32_t emptySince = 0, lastAdj = 0, lastData = millis();

  for (;;) {
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
        vTaskDelay(1);
        continue;
      }
    }

    // --- Buffer kosong saat play = underrun ---
    if (av == 0) {
      if (!emptySince) emptySince = now;
      if (now - emptySince > AUDIO_TIMEOUT_MS) {
        playing = false;
        S.active = false;
      }
      vTaskDelay(1);
      continue;
    }
    if (emptySince) {
      uint32_t gap = now - emptySince;
      S.underruns++;
      S.lastGap = gap;
      if (gap > S.maxGap) S.maxGap = gap;
      emptySince = 0;
    }

    size_t n = (av < CHUNK ? av : CHUNK) & ~3u;   // jaga alignment frame stereo 16-bit
    if (n < 4) { vTaskDelay(1); continue; }
    n = usbIn.readBytes(buf, n) & ~3u;
    if (n < 4) continue;

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

// ================== WEB MONITOR ==================
const char PAGE[] PROGMEM = R"HTML(<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>ESP32 DAC</title>
<style>
:root{color-scheme:dark light;--bg:#fff;--c:#f3f4f6;--t:#111;--m:#6b7280}
@media(prefers-color-scheme:dark){:root{--bg:#0f1115;--c:#1b1e25;--t:#eee;--m:#9aa0a6}}
body{margin:0;padding:12px;background:var(--bg);color:var(--t);font:15px system-ui,sans-serif}
h1{font-size:18px;margin:0 0 10px}.g{display:grid;grid-template-columns:1fr 1fr;gap:8px}
.c{background:var(--c);border-radius:10px;padding:10px}.c b{display:block;font-size:20px}
.c span{color:var(--m);font-size:12px}canvas{width:100%;height:90px;background:var(--c);border-radius:10px;margin-top:8px}
.dot{display:inline-block;width:10px;height:10px;border-radius:50%;background:#888;margin-right:6px}
</style></head><body>
<h1><span class="dot" id="d"></span><span id="st">...</span></h1>
<div class="g">
<div class="c"><span>Throughput</span><b id="kb">0 KB/s</b></div>
<div class="c"><span>Buffer USB</span><b id="lv">0%</b></div>
<div class="c"><span>Underrun</span><b id="un">0</b></div>
<div class="c"><span>Gap terakhir / maks</span><b id="gp">0 / 0 ms</b></div>
<div class="c"><span>Drift buang / gandakan</span><b id="dr">0 / 0</b></div>
<div class="c"><span>Uptime</span><b id="up">0s</b></div>
<div class="c"><span>Heap bebas</span><b id="hp">0 KB</b></div>
<div class="c"><span>WiFi klien</span><b id="cl">0</b></div>
<div class="c"><span>Reset reason (1=power,4=panic,7=wdt,15=brownout)</span><b id="rs">0</b></div>
</div>
<canvas id="c1" width="600" height="180"></canvas>
<canvas id="c2" width="600" height="180"></canvas>
<script>
const $=i=>document.getElementById(i);let pb=0,pt=0,H1=[],H2=[];
function draw(id,h,mx,col){const c=$(id),x=c.getContext('2d');x.clearRect(0,0,600,180);
x.strokeStyle=col;x.lineWidth=3;x.beginPath();h.forEach((v,i)=>{const X=i*600/59,Y=175-Math.min(v/mx,1)*170;i?x.lineTo(X,Y):x.moveTo(X,Y)});x.stroke();
x.fillStyle='#888';x.font='20px sans-serif';x.fillText(id=='c1'?'Throughput (KB/s)':'Buffer USB (%)',8,22)}
async function tick(){try{const s=await(await fetch('/stats')).json();
const dt=(s.ms-pt)/1000,kb=pt?(s.bytes-pb)/1024/dt:0;pb=s.bytes;pt=s.ms;
$('d').style.background=s.active?'#22c55e':'#888';$('st').textContent=s.active?'Memutar audio':'Idle';
const lv=Math.round(s.level*100/s.cap);
$('kb').textContent=kb.toFixed(0)+' KB/s';$('lv').textContent=lv+'%';$('un').textContent=s.under;
$('gp').textContent=s.gap+' / '+s.maxgap+' ms';$('dr').textContent=s.drops+' / '+s.dups;
$('up').textContent=Math.floor(s.up/60)+'m '+s.up%60+'s';$('hp').textContent=(s.heap/1024).toFixed(0)+' KB';$('cl').textContent=s.cl;$('rs').textContent=s.rst;
H1.push(kb);H2.push(lv);if(H1.length>60){H1.shift();H2.shift()}
draw('c1',H1,200,'#3b82f6');draw('c2',H2,100,'#f59e0b')}catch(e){$('st').textContent='Terputus'}}
setInterval(tick,500);tick();
</script></body></html>)HTML";

void handleStats() {
  char j[420];
  snprintf(j, sizeof(j),
    "{\"active\":%s,\"level\":%u,\"cap\":%u,\"bytes\":%llu,\"ms\":%lu,\"under\":%u,"
    "\"gap\":%u,\"maxgap\":%u,\"drops\":%u,\"dups\":%u,\"up\":%lu,\"heap\":%u,\"cl\":%d,\"rst\":%d}",
    S.active ? "true" : "false", (unsigned)S.level, (unsigned)FIFO_BYTES,
    (unsigned long long)S.bytes, (unsigned long)millis(), (unsigned)S.underruns,
    (unsigned)S.lastGap, (unsigned)S.maxGap, (unsigned)S.drops, (unsigned)S.dups,
    (unsigned long)(millis() / 1000), (unsigned)ESP.getFreeHeap(), WiFi.softAPgetStationNum(), (int)esp_reset_reason());
  server.send(200, "application/json", j);
}

// ================== SETUP / LOOP ==================
void setup() {
  // Tanpa Serial sama sekali. Alasan reset ditunjukkan lewat kedip LED:
  // 1x = power on normal, 2x = crash/panic, 3x = brownout, 4x = watchdog, 5x = lainnya
  pinMode(LED_PIN, OUTPUT);
  esp_reset_reason_t r = esp_reset_reason();
  int blinks = (r == ESP_RST_POWERON) ? 1 : (r == ESP_RST_PANIC) ? 2 :
               (r == ESP_RST_BROWNOUT) ? 3 :
               (r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_WDT) ? 4 : 5;
  for (int i = 0; i < blinks; i++) {
    digitalWrite(LED_PIN, HIGH); delay(250);
    digitalWrite(LED_PIN, LOW);  delay(250);
  }
  delay(600);

  if (!TinyUSBDevice.isInitialized()) TinyUSBDevice.begin(0);

  // USB Audio input
  auto usb_cfg = usbIn.defaultConfig(RX_MODE);
  usb_cfg.copyFrom(info);
  usb_cfg.manufacturer = "ESP32 Audio";
  usb_cfg.product = "ESP32-S2 DAC";
  usb_cfg.serial = "000001";
  usb_cfg.volume_active = false;      // bit-perfect, hindari scaling yang bikin noise
  usb_cfg.fifo_packets = FIFO_PACKETS;
  usbIn.begin(usb_cfg);

  // I2S output ke PCM5100A
  auto i2s_cfg = i2sOut.defaultConfig(TX_MODE);
  i2s_cfg.copyFrom(info);
  i2s_cfg.pin_bck = 16;
  i2s_cfg.pin_data = 17;
  i2s_cfg.pin_ws = 18;
  i2s_cfg.buffer_count = 8;
  i2s_cfg.buffer_size = 1024;
  i2sOut.begin(i2s_cfg);

  if (TinyUSBDevice.mounted()) {
    TinyUSBDevice.detach();
    delay(10);
    TinyUSBDevice.attach();
  }

#if ENABLE_WEB
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS, 1, 0, 2);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  server.on("/", []() { server.send_P(200, "text/html", PAGE); });
  server.on("/stats", handleStats);
  server.begin();
#endif

  // Task audio prioritas tinggi, terpisah dari loop() (web/LED)
  xTaskCreate(audioTask, "audio", 6144, nullptr, 3, nullptr);
}

void loop() {
#if ENABLE_WEB
  server.handleClient();
#endif
  digitalWrite(LED_PIN, S.active ? HIGH : LOW);
  delay(5);
}
