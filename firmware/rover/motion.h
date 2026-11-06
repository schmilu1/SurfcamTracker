// Bewegungssensor LIS3DH (I2C, Schaltplan TrackerD v1.2).
//
// 100 Hz, ±8 g, High-Resolution; die Messwerte laufen in den 32-stufigen FIFO des Sensors und
// werden alle 50 ms abgeholt. Pro Messwert:
//   - Schwerkraft per Tiefpass (Zeitkonstante ≈ 0,5 s) geschätzt
//   - dynamische Beschleunigung = Messwert - Schwerkraft (Paddeln, Vibration, Schläge)
//   - Neigung = Winkel zwischen Z-Achse und Schwerkraft
// Zusammengefasst wird getrennt für Funkpakete und Log (jeweils seit der letzten Abholung).
#pragma once

#include <Arduino.h>

struct MotionSummary {
  bool valid = false;
  uint16_t rmsMg = 0;   // RMS der dynamischen Beschleunigung
  uint16_t peakMg = 0;  // grösster Beschleunigungsbetrag (inkl. Schwerkraft)
  uint8_t tiltDeg = 0;  // 0 = Z-Achse nach oben, 180 = kopfüber
  uint16_t samples = 0;
};

class Motion {
 public:
  bool begin();
  void poll();   // regelmässig aufrufen
  void sleep();  // Sensor ausschalten (Deep Sleep)

  bool ok() const { return ok_; }
  uint8_t address() const { return addr_; }
  uint32_t samples() const { return samples_; }
  uint32_t overruns() const { return overruns_; }

  MotionSummary takeForPacket() { return take(packet_); }
  MotionSummary takeForLog() { return take(log_); }
  MotionSummary peekLive() const;  // aktueller Stand für Anzeige (ohne zurückzusetzen)
  void lastSampleMg(int16_t& x, int16_t& y, int16_t& z) const { x = ax_; y = ay_; z = az_; }

 private:
  struct Acc {
    float sumSq = 0;
    float peakSq = 0;
    uint16_t n = 0;
  };
  MotionSummary take(Acc& a);
  MotionSummary summarize(const Acc& a) const;
  bool writeReg(uint8_t reg, uint8_t val);
  bool readRegs(uint8_t reg, uint8_t* buf, size_t len);
  void addSample(int16_t x, int16_t y, int16_t z);

  bool ok_ = false;
  uint8_t addr_ = 0;
  uint32_t lastPollMs_ = 0;
  bool gravityInit_ = false;
  float gx_ = 0, gy_ = 0, gz_ = 0;  // Schwerkraft-Schätzung, mg
  int16_t ax_ = 0, ay_ = 0, az_ = 0;
  Acc packet_, log_, live_;
  uint32_t samples_ = 0;
  uint32_t overruns_ = 0;
};
