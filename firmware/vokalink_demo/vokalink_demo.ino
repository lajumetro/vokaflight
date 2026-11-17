// Demo VokaLink: data sikap & RC disimulasikan agar UI bisa dites tanpa sensor.
// Di FC asli: salin vokalink.h, isi VokaState dari IMU/RX/mixer, panggil voka.update().
#include <Preferences.h>
#include "vokalink.h"

VokaState st;
VokaLink voka(Serial, st);
Preferences prefs;

void vokaCalibrateAcc() {
  // ponytail: demo hanya reset sikap. FC asli: rata-rata sampel accel saat diam, simpan offset.
  st.roll = st.pitch = 0;
}

void vokaSavePid() {
  prefs.putBytes("pid", st.pid, sizeof(st.pid));
}

void setup() {
  Serial.begin(115200);
  prefs.begin("voka", false);
  if (prefs.getBytes("pid", st.pid, sizeof(st.pid)) != sizeof(st.pid)) {
    const float def[3][3] = {{45, 80, 30}, {47, 84, 34}, {45, 80, 0}};
    memcpy(st.pid, def, sizeof(def));
  }
}

void loop() {
  voka.update();

  float t = millis() / 1000.0f;
  st.roll = 20 * sinf(t * 0.7f);
  st.pitch = 12 * sinf(t * 0.5f + 1);
  st.yaw = fmodf(t * 15, 360) - 180;
  for (int i = 0; i < 8; i++) st.rc[i] = 1500 + 500 * sinf(t * (0.3f + i * 0.1f));

  // ponytail: demo tidak menggerakkan motor fisik. FC asli: tulis st.motor ke ESC.
  uint16_t test[4];
  if (voka.motorTest(test)) memcpy(st.motor, test, sizeof(test));
  else for (int i = 0; i < 4; i++) st.motor[i] = 1000;
}
