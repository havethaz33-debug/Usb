#include "AudioTools.h"

AudioInfo info(44100, 2, 16);
SineWaveGenerator<int16_t> sineWave(32000);
GeneratedSoundStream<int16_t> sound(sineWave);
I2SStream i2sOut;
StreamCopy copier(i2sOut, sound);

void setup() {
  Serial.begin(115200);
  
  // Inisialisasi generator nada 440Hz (Nada A)
  sineWave.begin(info, 440);

  // Konfigurasi Pin I2S
  auto i2s_cfg = i2sOut.defaultConfig(TX_MODE);
  i2s_cfg.copyFrom(info);
  i2s_cfg.pin_bck  = 16;
  i2s_cfg.pin_data = 17;
  i2s_cfg.pin_ws   = 18;
  i2sOut.begin(i2s_cfg);

  Serial.println("Memutar nada tes 440Hz ke DAC PCM5102...");
}

void loop() {
  copier.copy();
}
