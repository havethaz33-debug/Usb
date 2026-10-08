// ESP32-C3 + OLED SSD1306 128x64: animasi diupload lewat web (versi lengkap)
// Library: Adafruit SSD1306, Adafruit GFX, Adafruit BusIO
// (WiFi, WebServer, LittleFS sudah bawaan core ESP32)
//
// Cara pakai:
// 1. Sambungkan HP/laptop ke WiFi "OLED-Anim" (password 12345678)
// 2. Buka http://192.168.4.1
// 3. Pilih video (mp4/webm) atau beberapa gambar, atur FPS, klik "Konversi & Kirim"
// Konversi ke 1-bit 128x64 dilakukan di browser, lalu disimpan di flash
// (tetap ada setelah ESP32 dimatikan).

#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#define W 128
#define H 64
#define FRAME_BYTES 1024   // 128*64/8
#define OLED_ADDR 0x3C
#define SDA_PIN 8
#define SCL_PIN 9

const char* AP_SSID = "OLED-Anim";
const char* AP_PASS = "12345678";
const int AP_CHANNEL = 1;

Adafruit_SSD1306 display(W, H, &Wire, -1);
WebServer server(80);

File upFile, animFile;
volatile bool uploading = false;
uint8_t frameBuf[FRAME_BYTES];
uint8_t fps = 10;
uint32_t frameCount = 0, frameIdx = 0;
unsigned long lastFrame = 0;
String ipStr;

const char PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>OLED Animasi</title>
<style>
body{font-family:sans-serif;max-width:480px;margin:auto;padding:16px}
input,button{font-size:16px;margin:6px 0}
button{padding:10px 16px}
canvas{width:256px;height:128px;image-rendering:pixelated;background:#000;display:block;margin-top:8px}
</style></head><body>
<h2>Upload Animasi OLED</h2>
<p>Pilih 1 video (mp4/webm) atau beberapa gambar (dianggap frame, urut sesuai nama).</p>
<input type="file" id="f" accept="video/*,image/*" multiple><br>
FPS: <input type="number" id="fps" value="10" min="1" max="30" style="width:60px"><br>
Maks frame: <input type="number" id="mx" value="100" min="1" max="1000" style="width:80px"><br>
<label><input type="checkbox" id="inv"> Invert warna</label><br>
<button onclick="go()">Konversi &amp; Kirim</button>
<canvas id="c" width="128" height="64"></canvas>
<pre id="log"></pre>
<script>
const $=id=>document.getElementById(id);
const x=$('c').getContext('2d',{willReadFrequently:true});
const L=t=>$('log').textContent=t;
function grab(src,sw,sh){
  x.fillStyle='#000';x.fillRect(0,0,128,64);
  const s=Math.min(128/sw,64/sh),w=sw*s,h=sh*s;
  x.drawImage(src,(128-w)/2,(64-h)/2,w,h);
  const d=x.getImageData(0,0,128,64).data,out=new Uint8Array(1024),inv=$('inv').checked;
  for(let i=0;i<8192;i++){
    const p=i*4;
    let v=(d[p]*0.299+d[p+1]*0.587+d[p+2]*0.114)>128;
    if(inv)v=!v;
    if(v)out[i>>3]|=0x80>>(i&7);
  }
  return out;
}
async function go(){
  const files=[...$('f').files];
  if(!files.length)return L('Pilih file dulu');
  const fp=+$('fps').value,mx=+$('mx').value,frames=[];
  try{
    if(files[0].type.startsWith('video')){
      const v=document.createElement('video');
      v.muted=true;v.preload='auto';v.src=URL.createObjectURL(files[0]);
      await new Promise(r=>v.onloadedmetadata=r);
      const n=Math.min(mx,Math.floor(v.duration*fp));
      for(let i=0;i<n;i++){
        const p=new Promise(r=>v.onseeked=r);
        v.currentTime=(i+0.5)/fp;
        await p;
        frames.push(grab(v,v.videoWidth,v.videoHeight));
        L('Konversi '+(i+1)+'/'+n);
      }
    }else{
      files.sort((a,b)=>a.name.localeCompare(b.name,undefined,{numeric:true}));
      const list=files.slice(0,mx);
      for(let i=0;i<list.length;i++){
        const b=await createImageBitmap(list[i]);
        frames.push(grab(b,b.width,b.height));
        L('Konversi '+(i+1)+'/'+list.length);
      }
    }
    if(!frames.length)return L('Tidak ada frame');
    const data=new Uint8Array(1+frames.length*1024);
    data[0]=fp;
    frames.forEach((fr,i)=>data.set(fr,1+i*1024));
    const fd=new FormData();
    fd.append('file',new Blob([data]),'anim.bin');
    L('Mengirim '+frames.length+' frame...');
    const r=await fetch('/upload',{method:'POST',body:fd});
    L(r.ok?'Selesai! '+frames.length+' frame terkirim.':'Gagal kirim');
  }catch(e){L('Error: '+e);}
}
</script></body></html>
)rawliteral";

void showText(const char* line1, const char* line2 = "", const char* line3 = "") {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println(line1);
  display.println(line2);
  display.println(line3);
  display.display();
}

void openAnim() {
  if (animFile) animFile.close();
  frameCount = 0;
  frameIdx = 0;
  if (!LittleFS.exists("/anim.bin")) return;
  animFile = LittleFS.open("/anim.bin", "r");
  if (!animFile || animFile.size() < 1 + FRAME_BYTES) return;
  fps = animFile.read();
  if (fps < 1 || fps > 30) fps = 10;
  frameCount = (animFile.size() - 1) / FRAME_BYTES;
}

void handleUpload() {
  HTTPUpload& u = server.upload();
  if (u.status == UPLOAD_FILE_START) {
    uploading = true;
    if (animFile) animFile.close();
    frameCount = 0;
    upFile = LittleFS.open("/anim.bin", "w");
    showText("Menerima animasi...");
  } else if (u.status == UPLOAD_FILE_WRITE) {
    if (upFile) upFile.write(u.buf, u.currentSize);
  } else if (u.status == UPLOAD_FILE_END) {
    if (upFile) upFile.close();
  } else if (u.status == UPLOAD_FILE_ABORTED) {
    if (upFile) upFile.close();
    uploading = false;
  }
}

void handleDone() {
  server.send(200, "text/plain", "OK");
  openAnim();
  uploading = false;
}

void playFrame() {
  if (uploading || frameCount == 0) return;
  if (millis() - lastFrame < 1000UL / fps) return;
  lastFrame = millis();

  animFile.seek(1 + (uint32_t)frameIdx * FRAME_BYTES);
  animFile.read(frameBuf, FRAME_BYTES);

  display.clearDisplay();
  display.drawBitmap(0, 0, frameBuf, W, H, SSD1306_WHITE);
  display.display();

  frameIdx = (frameIdx + 1) % frameCount;
}

void setup() {
  Serial.begin(115200);
  Wire.begin(SDA_PIN, SCL_PIN);

  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("OLED tidak terdeteksi!");
    while (true) delay(1000);
  }

  if (!LittleFS.begin(true)) {
    showText("LittleFS gagal");
    while (true) delay(1000);
  }

  // WiFi Access Point (daya pancar diturunkan supaya stabil di board C3 kecil)
  WiFi.mode(WIFI_AP);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  bool apOk = WiFi.softAP(AP_SSID, AP_PASS, AP_CHANNEL);
  if (!apOk) {
    showText("WiFi AP gagal");
    Serial.println("softAP gagal");
    while (true) delay(1000);
  }
  ipStr = WiFi.softAPIP().toString();
  Serial.println("AP: " + String(AP_SSID) + "  IP: " + ipStr);

  server.on("/", HTTP_GET, []() { server.send_P(200, "text/html", PAGE); });
  server.on("/upload", HTTP_POST, handleDone, handleUpload);
  server.begin();

  openAnim();
  if (frameCount == 0) {
    showText("WiFi: OLED-Anim", ("Buka " + ipStr).c_str());
  }
}

void loop() {
  server.handleClient();
  playFrame();
}
