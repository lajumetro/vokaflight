// VokaLink + MPU6050 (ESP32). Tanpa library tambahan, hanya Wire.
// Wiring: VCC 3V3, GND GND, SDA GPIO21, SCL GPIO22, AD0 ke GND (alamat 0x68).
// Saat boot, diamkan board rata ~1 detik untuk kalibrasi gyro.
#include <Preferences.h>
#include <Wire.h>
#include "vokalink.h"

constexpr int PIN_SDA = 21;
constexpr int PIN_SCL = 22;
constexpr int PIN_LED = 2;
constexpr float GYRO_LSB = 65.5f;    // +-500 dps
constexpr float ACC_LSB = 8192.0f;   // +-4 g
constexpr float ALPHA = 0.98f;       // bobot gyro pada complementary filter

VokaState st;
VokaLink voka(Serial, st);
Preferences prefs;

float gyroBias[3] = {};
float accOff[2] = {};   // offset roll, pitch (derajat)
float rollRaw = 0, pitchRaw = 0;
bool imuOk = false;
uint32_t lastUs = 0;
uint32_t lastInitTry = 0;
uint8_t MPU_ADDR = 0x68;   // 0x68 (AD0=GND) atau 0x69 (AD0=3V3), dideteksi otomatis

struct Imu { float ax, ay, az, gx, gy, gz; };

bool mpuWrite(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

bool mpuRead(Imu& d) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x3B);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)MPU_ADDR, 14) != 14) return false;
  int16_t v[7];
  for (int i = 0; i < 7; i++) {
    uint8_t h = Wire.read(), l = Wire.read();
    v[i] = (int16_t)((h << 8) | l);
  }
  d.ax = v[0] / ACC_LSB; d.ay = v[1] / ACC_LSB; d.az = v[2] / ACC_LSB;
  // v[3] = suhu, dilewati
  d.gx = v[4] / GYRO_LSB; d.gy = v[5] / GYRO_LSB; d.gz = v[6] / GYRO_LSB;
  return true;
}

float accRoll(const Imu& d) { return atan2f(d.ay, d.az) * RAD_TO_DEG; }
float accPitch(const Imu& d) { return atan2f(-d.ax, sqrtf(d.ay * d.ay + d.az * d.az)) * RAD_TO_DEG; }

void calibrateGyro() {
  float sum[3] = {};
  int n = 0;
  Imu d;
  for (int i = 0; i < 500; i++) {
    if (mpuRead(d)) { sum[0] += d.gx; sum[1] += d.gy; sum[2] += d.gz; n++; }
    delay(2);
  }
  if (n) for (int k = 0; k < 3; k++) gyroBias[k] = sum[k] / n;
}

// Dipanggil VokaLink saat PC kirim 'calacc'. Board harus diam dan rata. Memblokir ~0.5 dtk.
void vokaCalibrateAcc() {
  float r = 0, p = 0;
  int n = 0;
  Imu d;
  for (int i = 0; i < 200; i++) {
    if (mpuRead(d)) { r += accRoll(d); p += accPitch(d); n++; }
    delay(2);
  }
  if (!n) return;
  accOff[0] = r / n;
  accOff[1] = p / n;
  prefs.putBytes("accoff", accOff, sizeof(accOff));
  rollRaw = pitchRaw = 0;
}

// LED tanpa delay(), hanya membaca millis().
//  IMU gagal       : kedip sangat cepat (50 ms)
//  Menunggu PC     : kedip singkat tiap 1 detik
//  Menghubungkan   : kedip cepat (100 ms)
//  Terhubung       : kedip cepat tiap ada data TX/RX (seperti LED USB-serial)
void updateLed() {
  uint32_t t = millis();
  bool on;
  if (!imuOk) on = (t / 50) % 2;
  else switch (voka.state()) {
    case VokaLink::CONNECTED:  on = (t - voka.lastActivity()) < 20; break;  // kedip tiap ada TX/RX
    case VokaLink::CONNECTING: on = (t / 100) % 2; break;
    default:                   on = (t % 1000) < 100;
  }
  digitalWrite(PIN_LED, on);
}

// Coba 0x68 lalu 0x69. Bila gagal, kirim hasil scan I2C ke log di UI (baris 'ERR ...').
bool initImu() {
  for (uint8_t a : {(uint8_t)0x68, (uint8_t)0x69}) {
    MPU_ADDR = a;
    if (mpuWrite(0x6B, 0x00)) {          // bangunkan dari sleep
      delay(50);
      if (mpuWrite(0x1A, 0x03)           // DLPF ~44 Hz
          && mpuWrite(0x1B, 0x08)        // gyro +-500 dps
          && mpuWrite(0x1C, 0x08)) {     // accel +-4 g
        calibrateGyro();
        Imu d;
        if (mpuRead(d)) { rollRaw = accRoll(d); pitchRaw = accPitch(d); }
        return true;
      }
    }
  }
  Serial.print("ERR imu not found. I2C devices:");
  int n = 0;
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) { Serial.printf(" 0x%02X", a); n++; }
  }
  Serial.println(n ? "" : " none (check SDA/SCL/VCC/GND)");
  return false;
}

void vokaSavePid() {
  prefs.putBytes("pid", st.pid, sizeof(st.pid));
}

void setup() {
  pinMode(PIN_LED, OUTPUT);
  Serial.begin(115200);
  prefs.begin("voka", false);
  if (prefs.getBytes("pid", st.pid, sizeof(st.pid)) != sizeof(st.pid)) {
    const float def[3][3] = {{45, 80, 30}, {47, 84, 34}, {45, 80, 0}};
    memcpy(st.pid, def, sizeof(def));
  }
  if (prefs.getBytes("accoff", accOff, sizeof(accOff)) != sizeof(accOff)) accOff[0] = accOff[1] = 0;

  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(100000);
  Wire.setTimeOut(20);
  imuOk = initImu();
  lastInitTry = millis();
  for (int i = 0; i < 8; i++) st.rc[i] = 1500;
  for (int i = 0; i < 4; i++) st.motor[i] = 1000;
  lastUs = micros();
}

void loop() {
  voka.update();
  updateLed();

  // IMU belum ada: coba lagi tiap 2 detik (kabel baru dicolok, dst).
  if (!imuOk && millis() - lastInitTry > 2000) {
    lastInitTry = millis();
    imuOk = initImu();
  }

  Imu d;
  if (imuOk && mpuRead(d)) {
    uint32_t now = micros();
    float dt = (now - lastUs) * 1e-6f;
    lastUs = now;
    if (dt <= 0 || dt > 0.1f) dt = 0.004f;

    float gx = d.gx - gyroBias[0], gy = d.gy - gyroBias[1], gz = d.gz - gyroBias[2];
    rollRaw = ALPHA * (rollRaw + gx * dt) + (1 - ALPHA) * accRoll(d);
    pitchRaw = ALPHA * (pitchRaw + gy * dt) + (1 - ALPHA) * accPitch(d);
    st.roll = rollRaw - accOff[0];
    st.pitch = -(pitchRaw - accOff[1]);   // konvensi VokaLink: pitch positif = hidung naik

    // Yaw dari integrasi gyro saja (tanpa magnetometer), jadi akan melayang pelan.
    float y = st.yaw + gz * dt;
    if (y > 180) y -= 360;
    if (y < -180) y += 360;
    st.yaw = y;
  }

  // ponytail: belum ada receiver RC & ESC. FC asli: isi st.rc dari RX, tulis st.motor ke ESC.
  uint16_t test[4];
  if (voka.motorTest(test)) memcpy(st.motor, test, sizeof(test));
  else for (int i = 0; i < 4; i++) st.motor[i] = 1000;
}
