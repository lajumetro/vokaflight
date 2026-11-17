// VokaLink: protokol serial teks antara FC ESP32 dan VOKAFLIGHT.
// Satu perintah per baris, diakhiri '\n', 115200 baud.
// PC ke FC: hello | ping | stream 0/1 | pid? | pid <axis 0..2> <P> <I> <D> | save | calacc | mt <m1..m4 1000..2000>
// FC ke PC: I <nama> <versi> | A <roll> <pitch> <yaw> <armed> | R <ch1..ch8> | M <m1..m4>
//           P <axis> <P> <I> <D> | OK <cmd> | ERR <alasan>
#pragma once
#include <Arduino.h>

struct VokaState {
  float roll = 0, pitch = 0, yaw = 0;  // derajat
  uint16_t rc[8] = {};                 // mikrodetik
  uint16_t motor[4] = {};              // mikrodetik, output aktual
  float pid[3][3] = {};                // [roll,pitch,yaw][P,I,D]
  bool armed = false;
};

// Wajib diimplementasi oleh firmware FC.
void vokaCalibrateAcc();
void vokaSavePid();

class VokaLink {
 public:
  static constexpr uint32_t MOTOR_TEST_TIMEOUT_MS = 500;
  static constexpr uint32_t HOST_TIMEOUT_MS = 2000;  // tanpa baris dari PC selama ini = PC dianggap hilang
  static constexpr uint32_t STREAM_PERIOD_MS = 40;  // 25 Hz, ~2.3 kB/s dari 11.5 kB/s @115200

  enum LinkState { IDLE, CONNECTING, CONNECTED };

  VokaLink(Stream& io, VokaState& st) : io_(io), st_(st) {}

  // Waktu (millis) terakhir ada data keluar (telemetri) atau masuk (perintah). Untuk LED ala TX/RX.
  uint32_t lastActivity() const { return lastActivity_; }

  // IDLE: tidak ada PC. CONNECTING: PC menyapa (hello) tapi stream belum jalan. CONNECTED: stream aktif.
  LinkState state() const {
    if (!hostSeen_ || millis() - lastHost_ > HOST_TIMEOUT_MS) return IDLE;
    return streaming_ ? CONNECTED : CONNECTING;
  }

  // Panggil setiap loop. Tidak blocking.
  void update() {
    while (io_.available()) {
      char c = io_.read();
      if (c == '\r') continue;
      if (c == '\n') {
        if (!overflow_) { buf_[len_] = 0; handle(buf_); }
        len_ = 0; overflow_ = false;
      } else if (len_ < sizeof(buf_) - 1) {
        buf_[len_++] = c;
      } else {
        overflow_ = true;
      }
    }
    uint32_t now = millis();
    if (streaming_ && state() == IDLE) streaming_ = false;  // PC hilang: berhenti mengirim
    if (streaming_ && now - lastStream_ >= STREAM_PERIOD_MS) {
      lastStream_ = now;
      lastActivity_ = now;
      sendTelemetry();
    }
  }

  // True jika tes motor aktif; out diisi nilai tes. Mati sendiri bila
  // konfigurator berhenti mengirim 'mt' (kabel lepas, tab ditutup) atau FC arm.
  bool motorTest(uint16_t out[4]) {
    if (st_.armed || !mtActive_ || millis() - mtStamp_ > MOTOR_TEST_TIMEOUT_MS) {
      mtActive_ = false;
      return false;
    }
    memcpy(out, mt_, sizeof(mt_));
    return true;
  }

 private:
  Stream& io_;
  VokaState& st_;
  char buf_[96];
  size_t len_ = 0;
  bool overflow_ = false;
  bool streaming_ = false;
  bool hostSeen_ = false;
  uint32_t lastActivity_ = 0;
  uint32_t lastHost_ = 0;
  uint32_t lastStream_ = 0;
  uint16_t mt_[4] = {};
  bool mtActive_ = false;
  uint32_t mtStamp_ = 0;

  void sendTelemetry() {
    io_.printf("A %.1f %.1f %.1f %d\n", st_.roll, st_.pitch, st_.yaw, st_.armed ? 1 : 0);
    io_.printf("R %u %u %u %u %u %u %u %u\n", st_.rc[0], st_.rc[1], st_.rc[2], st_.rc[3],
               st_.rc[4], st_.rc[5], st_.rc[6], st_.rc[7]);
    io_.printf("M %u %u %u %u\n", st_.motor[0], st_.motor[1], st_.motor[2], st_.motor[3]);
  }

  void sendPid() {
    for (int a = 0; a < 3; a++)
      io_.printf("P %d %.3f %.3f %.3f\n", a, st_.pid[a][0], st_.pid[a][1], st_.pid[a][2]);
  }

  void handle(const char* line) {
    int axis, on;
    float p, i, d;
    unsigned m[4];
    hostSeen_ = true;
    lastHost_ = lastActivity_ = millis();
    if (!strcmp(line, "ping")) {
      return;  // hanya penanda PC masih hidup
    } else if (!strcmp(line, "hello")) {
      io_.println("I VOKAFLIGHT 1");
      sendPid();
    } else if (sscanf(line, "stream %d", &on) == 1) {
      streaming_ = on;
      io_.println("OK stream");
    } else if (!strcmp(line, "pid?")) {
      sendPid();
    } else if (sscanf(line, "pid %d %f %f %f", &axis, &p, &i, &d) == 4) {
      if (axis < 0 || axis > 2 || p < 0 || i < 0 || d < 0 || p > 500 || i > 500 || d > 500) {
        io_.println("ERR pid range");
        return;
      }
      st_.pid[axis][0] = p; st_.pid[axis][1] = i; st_.pid[axis][2] = d;
      io_.println("OK pid");
    } else if (!strcmp(line, "save")) {
      if (st_.armed) { io_.println("ERR armed"); return; }
      vokaSavePid();
      io_.println("OK save");
    } else if (!strcmp(line, "calacc")) {
      if (st_.armed) { io_.println("ERR armed"); return; }
      vokaCalibrateAcc();
      io_.println("OK calacc");
    } else if (sscanf(line, "mt %u %u %u %u", &m[0], &m[1], &m[2], &m[3]) == 4) {
      if (st_.armed) { io_.println("ERR armed"); return; }
      for (int k = 0; k < 4; k++) {
        if (m[k] < 1000 || m[k] > 2000) { io_.println("ERR mt range"); return; }
      }
      for (int k = 0; k < 4; k++) mt_[k] = m[k];
      mtActive_ = true;
      mtStamp_ = millis();
    } else {
      io_.printf("ERR unknown: %s\n", line);
    }
  }
};
