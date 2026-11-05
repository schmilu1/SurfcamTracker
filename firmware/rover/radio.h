// LoRa Punkt-zu-Punkt über den SX1276 (Bibliothek RadioLib)
// mit Einhaltung des Duty Cycle (10 % im Band 869.40-869.65 MHz, sonst 1 %).
#pragma once

#include <stddef.h>
#include <stdint.h>

class Radio {
 public:
  bool begin(uint32_t freqHz, uint8_t sf, uint32_t bwHz, int8_t txPowerDbm);
  bool ok() const { return ok_; }

  // true, wenn der Duty Cycle eine neue Sendung erlaubt
  bool canSend() const;
  // Sendet blockierend (ca. 26 ms bei SF7/250 kHz) und legt den Chip danach schlafen.
  bool send(const uint8_t* data, size_t len);
  void sleep();

  uint32_t airtimeUs(size_t len) const;
  uint32_t minSpacingMs(size_t len) const;
  uint16_t dutyPermille() const { return dutyPermille_; }

  uint32_t txCount() const { return txCount_; }
  uint32_t txFailed() const { return txFailed_; }
  // Mittlerer Duty Cycle seit dem Start in Prozent
  float averageDutyPercent() const;

 private:
  bool ok_ = false;
  uint32_t freqHz_ = 0;
  uint32_t bwHz_ = 0;
  uint8_t sf_ = 7;
  uint16_t dutyPermille_ = 10;
  uint32_t lastTxStartMs_ = 0;
  uint32_t requiredGapMs_ = 0;
  uint32_t txCount_ = 0;
  uint32_t txFailed_ = 0;
  uint64_t airtimeTotalUs_ = 0;
  uint32_t startMs_ = 0;
};
