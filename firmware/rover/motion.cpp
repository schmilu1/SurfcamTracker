#include "motion.h"

#include <Wire.h>
#include <math.h>

#include "config.h"

namespace {
constexpr uint8_t REG_WHO_AM_I = 0x0F;  // 0x33
constexpr uint8_t REG_CTRL1 = 0x20;
constexpr uint8_t REG_CTRL4 = 0x23;
constexpr uint8_t REG_CTRL5 = 0x24;
constexpr uint8_t REG_OUT_X_L = 0x28;
constexpr uint8_t REG_FIFO_CTRL = 0x2E;
constexpr uint8_t REG_FIFO_SRC = 0x2F;
constexpr uint8_t AUTO_INC = 0x80;

constexpr float kGravityAlpha = 0.02f;  // 100 Hz -> Zeitkonstante ≈ 0,5 s
constexpr uint32_t kPollMs = 50;        // FIFO fasst 32 Werte = 320 ms
constexpr int kMgPerDigit = 4;          // ±8 g, High-Resolution (12 Bit)
}  // namespace

bool Motion::writeReg(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(addr_);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

bool Motion::readRegs(uint8_t reg, uint8_t* buf, size_t len) {
  Wire.beginTransmission(addr_);
  Wire.write(len > 1 ? (reg | AUTO_INC) : reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(addr_, (uint8_t)len) != len) return false;
  for (size_t i = 0; i < len; i++) buf[i] = (uint8_t)Wire.read();
  return true;
}

bool Motion::begin() {
  ok_ = false;
  Wire.begin(pins::I2C_SDA, pins::I2C_SCL, 400000);
  // Der erste Zugriff nach dem Einschalten schlägt gelegentlich fehl: mehrere Versuche
  for (int attempt = 0; attempt < 5 && !ok_; attempt++) {
    if (attempt) delay(20);
    for (uint8_t a : {0x19, 0x18}) {  // SA0 = 1 bzw. 0
      addr_ = a;
      uint8_t id = 0;
      if (readRegs(REG_WHO_AM_I, &id, 1) && id == 0x33) {
        ok_ = true;
        if (attempt) Serial.printf("[motion] gefunden im %d. Versuch\r\n", attempt + 1);
        break;
      }
    }
  }
  if (!ok_) {
    addr_ = 0;
    Serial.printf("[motion] kein LIS3DH an SDA %d / SCL %d gefunden. I2C-Geräte:", pins::I2C_SDA,
                  pins::I2C_SCL);
    for (uint8_t a = 1; a < 127; a++) {
      Wire.beginTransmission(a);
      if (Wire.endTransmission() == 0) {
        addr_ = a;
        uint8_t id = 0;
        const bool r = readRegs(REG_WHO_AM_I, &id, 1);
        Serial.printf(" 0x%02X (WHO_AM_I %s0x%02X)", a, r ? "" : "Lesefehler ", id);
      }
    }
    addr_ = 0;
    Serial.println();
    return false;
  }

  ok_ = writeReg(REG_CTRL1, 0x57)          // 100 Hz, normaler Modus, X/Y/Z an
        && writeReg(REG_CTRL4, 0xA8)       // BDU, ±8 g, High-Resolution
        && writeReg(REG_CTRL5, 0x40)       // FIFO an
        && writeReg(REG_FIFO_CTRL, 0x00)   // Bypass (FIFO leeren)
        && writeReg(REG_FIFO_CTRL, 0x80);  // Stream-Modus
  Serial.printf("[motion] LIS3DH %s (0x%02X), 100 Hz, ±8 g\r\n", ok_ ? "OK" : "FEHLER", addr_);
  lastPollMs_ = millis();
  return ok_;
}

void Motion::sleep() {
  if (addr_) writeReg(REG_CTRL1, 0x00);  // Power-down
}

void Motion::addSample(int16_t x, int16_t y, int16_t z) {
  ax_ = x;
  ay_ = y;
  az_ = z;
  if (!gravityInit_) {
    gx_ = x;
    gy_ = y;
    gz_ = z;
    gravityInit_ = true;
  } else {
    gx_ += (x - gx_) * kGravityAlpha;
    gy_ += (y - gy_) * kGravityAlpha;
    gz_ += (z - gz_) * kGravityAlpha;
  }
  const float dx = x - gx_, dy = y - gy_, dz = z - gz_;
  const float dynSq = dx * dx + dy * dy + dz * dz;
  const float magSq = (float)x * x + (float)y * y + (float)z * z;
  for (Acc* a : {&packet_, &log_, &live_}) {
    a->sumSq += dynSq;
    if (magSq > a->peakSq) a->peakSq = magSq;
    if (a->n < UINT16_MAX) a->n++;
  }
  samples_++;
}

void Motion::poll() {
  if (!ok_ || millis() - lastPollMs_ < kPollMs) return;
  lastPollMs_ = millis();
  uint8_t src;
  if (!readRegs(REG_FIFO_SRC, &src, 1)) return;
  if (src & 0x40) overruns_++;
  uint8_t n = src & 0x1F;
  if (src & 0x40 && n == 0) n = 32;
  uint8_t buf[6];
  while (n--) {
    if (!readRegs(REG_OUT_X_L, buf, sizeof(buf))) return;
    // 12 Bit linksbündig
    const int16_t x = (int16_t)(buf[0] | buf[1] << 8) >> 4;
    const int16_t y = (int16_t)(buf[2] | buf[3] << 8) >> 4;
    const int16_t z = (int16_t)(buf[4] | buf[5] << 8) >> 4;
    addSample(x * kMgPerDigit, y * kMgPerDigit, z * kMgPerDigit);
  }
  // Anzeige: gleitend über ≈ 1 s
  if (live_.n >= 100) {
    live_.sumSq *= 0.5f;
    live_.n /= 2;
    live_.peakSq *= 0.9f;
  }
}

MotionSummary Motion::summarize(const Acc& a) const {
  MotionSummary s;
  if (!ok_ || a.n == 0) return s;
  s.valid = true;
  s.samples = a.n;
  s.rmsMg = (uint16_t)min(65535.0f, sqrtf(a.sumSq / a.n));
  s.peakMg = (uint16_t)min(65535.0f, sqrtf(a.peakSq));
  const float g = sqrtf(gx_ * gx_ + gy_ * gy_ + gz_ * gz_);
  if (g > 1) {
    float c = gz_ / g;
    c = c > 1 ? 1 : c < -1 ? -1 : c;
    s.tiltDeg = (uint8_t)lroundf(acosf(c) * 180.0f / (float)M_PI);
  }
  return s;
}

MotionSummary Motion::take(Acc& a) {
  MotionSummary s = summarize(a);
  a = Acc();
  return s;
}

MotionSummary Motion::peekLive() const { return summarize(live_); }
