#include "radio.h"

#include <Arduino.h>
#include <LoRa.h>
#include <SPI.h>

#include "config.h"
#include "packet.h"

bool Radio::begin(uint32_t freqHz, uint8_t sf, uint32_t bwHz, int8_t txPowerDbm) {
  freqHz_ = freqHz;
  sf_ = sf;
  bwHz_ = bwHz;
  dutyPermille_ = surfcam::dutyCyclePermille(freqHz, bwHz);
  startMs_ = millis();
  if (ok_) LoRa.end();  // erneute Konfiguration per AT-Befehl

  SPI.begin(pins::LORA_SCK, pins::LORA_MISO, pins::LORA_MOSI, pins::LORA_NSS);
  LoRa.setSPI(SPI);
  LoRa.setPins(pins::LORA_NSS, pins::LORA_RST, pins::LORA_DIO0);
  if (!LoRa.begin((long)freqHz)) {
    Serial.println("[radio] SX1276 nicht gefunden");
    ok_ = false;
    return false;
  }
  LoRa.setSpreadingFactor(sf);
  LoRa.setSignalBandwidth((long)bwHz);
  LoRa.setCodingRate4(5);
  LoRa.setPreambleLength(8);
  LoRa.setSyncWord(LORA_SYNC_WORD);
  LoRa.enableCrc();
  LoRa.disableInvertIQ();  // Uplink-Polarität wie bei LoRaWAN
  LoRa.setTxPower(txPowerDbm, PA_OUTPUT_PA_BOOST_PIN);
  LoRa.sleep();
  ok_ = true;

  Serial.printf("[radio] OK, %.3f MHz, SF%u, %lu kHz, %d dBm, Duty Cycle %u %%, Paket %lu us, min. Abstand %lu ms\r\n",
                freqHz / 1e6, sf, (unsigned long)(bwHz / 1000), txPowerDbm, dutyPermille_ / 10,
                (unsigned long)airtimeUs(surfcam::kPacketSize),
                (unsigned long)minSpacingMs(surfcam::kPacketSize));
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

  bool done = LoRa.beginPacket() && LoRa.write(data, len) == len && LoRa.endPacket();  // blockierend
  LoRa.sleep();
  airtimeTotalUs_ += air;  // auch bei Fehler konservativ mitzählen
  if (done) {
    txCount_++;
  } else {
    txFailed_++;
  }
  return done;
}

void Radio::sleep() {
  if (ok_) LoRa.sleep();
}

float Radio::averageDutyPercent() const {
  const uint32_t elapsedMs = millis() - startMs_;
  if (elapsedMs == 0) return 0;
  return (float)(airtimeTotalUs_ / 10.0 / elapsedMs);
}
