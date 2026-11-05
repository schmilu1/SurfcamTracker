#include "radio.h"

#include <Arduino.h>
#include <RadioLib.h>
#include <SPI.h>

#include "config.h"
#include "packet.h"

// SX1276 (RFM95-kompatibel) an VSPI mit eigener Pinbelegung.
// SPI.begin() mit den TrackerD-Pins wird vor lora.begin() aufgerufen; der spätere
// SPI.begin() ohne Pins in RadioLib ändert die Pinbelegung dann nicht mehr.
static SX1276 lora = new Module(pins::LORA_NSS, pins::LORA_DIO0, pins::LORA_RST, pins::LORA_DIO1, SPI);

static const char* errText(int16_t state) {
  switch (state) {
    case RADIOLIB_ERR_NONE: return "OK";
    case RADIOLIB_ERR_CHIP_NOT_FOUND: return "Chip nicht gefunden";
    case RADIOLIB_ERR_TX_TIMEOUT: return "Sende-Timeout";
    case RADIOLIB_ERR_INVALID_FREQUENCY: return "ungültige Frequenz";
    case RADIOLIB_ERR_INVALID_BANDWIDTH: return "ungültige Bandbreite";
    case RADIOLIB_ERR_INVALID_SPREADING_FACTOR: return "ungültiger Spreading Factor";
    case RADIOLIB_ERR_INVALID_OUTPUT_POWER: return "ungültige Sendeleistung";
    default: return "Fehler";
  }
}

bool Radio::begin(uint32_t freqHz, uint8_t sf, uint32_t bwHz, int8_t txPowerDbm) {
  freqHz_ = freqHz;
  sf_ = sf;
  bwHz_ = bwHz;
  dutyPermille_ = surfcam::dutyCyclePermille(freqHz, bwHz);
  startMs_ = millis();
  ok_ = false;

  SPI.begin(pins::LORA_SCK, pins::LORA_MISO, pins::LORA_MOSI, pins::LORA_NSS);

  // Frequenz in MHz, Bandbreite in kHz, Coding Rate 4/5, Präambel 8 Symbole
  int16_t state = lora.begin(freqHz / 1e6f, bwHz / 1000.0f, sf, 5, LORA_SYNC_WORD, txPowerDbm, 8);
  if (state == RADIOLIB_ERR_NONE) state = lora.setCRC(true);
  if (state == RADIOLIB_ERR_NONE) state = lora.invertIQ(false);  // Uplink-Polarität wie bei LoRaWAN
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("[radio] SX1276: %s (%d)\r\n", errText(state), state);
    return false;
  }
  lora.sleep();
  ok_ = true;

  const uint32_t ownUs = airtimeUs(surfcam::kPacketSize);
  const uint32_t libUs = (uint32_t)lora.getTimeOnAir(surfcam::kPacketSize);
  Serial.printf("[radio] OK, %.3f MHz, SF%u, %lu kHz, %d dBm, Duty Cycle %u %%, Paket %lu us, min. Abstand %lu ms\r\n",
                freqHz / 1e6, sf, (unsigned long)(bwHz / 1000), txPowerDbm, dutyPermille_ / 10,
                (unsigned long)ownUs, (unsigned long)minSpacingMs(surfcam::kPacketSize));
  // Plausibilitätsprüfung der eigenen Sendezeit-Berechnung gegen RadioLib
  const uint32_t diff = ownUs > libUs ? ownUs - libUs : libUs - ownUs;
  if (diff > ownUs / 20) {
    Serial.printf("[radio] WARNUNG: Sendezeit eigene Berechnung %lu us, RadioLib %lu us\r\n",
                  (unsigned long)ownUs, (unsigned long)libUs);
  }
  return true;
}

uint32_t Radio::airtimeUs(size_t len) const { return surfcam::loraAirtimeUs(len, sf_, bwHz_); }

uint32_t Radio::minSpacingMs(size_t len) const {
  return surfcam::minTxSpacingMs(airtimeUs(len), dutyPermille_);
}

bool Radio::canSend() const {
  if (!ok_) return false;
  if (txCount_ == 0 && txFailed_ == 0) return true;
  return millis() - lastTxStartMs_ >= requiredGapMs_;
}

bool Radio::send(const uint8_t* data, size_t len) {
  if (!canSend()) return false;
  const uint32_t air = airtimeUs(len);
  lastTxStartMs_ = millis();
  // Abstand nach dieser Sendung, damit Sendezeit / Zeitraum <= Duty Cycle bleibt
  requiredGapMs_ = surfcam::minTxSpacingMs(air, dutyPermille_);

  const int16_t state = lora.transmit(data, len);  // blockierend
  lora.sleep();
  airtimeTotalUs_ += air;  // auch bei Fehler konservativ mitzählen
  if (state == RADIOLIB_ERR_NONE) {
    txCount_++;
    return true;
  }
  txFailed_++;
  if (txFailed_ <= 5 || txFailed_ % 100 == 0) {
    Serial.printf("[radio] Senden fehlgeschlagen: %s (%d)\r\n", errText(state), state);
  }
  return false;
}

void Radio::sleep() {
  if (ok_) lora.sleep();
}

float Radio::averageDutyPercent() const {
  const uint32_t elapsedMs = millis() - startMs_;
  if (elapsedMs == 0) return 0;
  return (float)(airtimeTotalUs_ / 10.0 / elapsedMs);
}
