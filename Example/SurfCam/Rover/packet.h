// SurfCam-Funkpaket (LoRa Punkt-zu-Punkt) – reines C++, ohne Arduino-Abhängigkeit.
//
// Aufbau (19 Bytes, Big Endian):
//   0      MHDR 0xE0 (LoRaWAN "Proprietary"; Netzwerkserver verwerfen solche Frames)
//   1      Bits 7..5 Version (1), Bits 4..0 Geräte-ID (0..31)
//   2      Sequenznummer (0..255, läuft über)
//   3..6   Bits 31..27 Akku-Stufe (0..31 -> 3000 + 40*n mV), Bits 26..0 Zeit des Fixes in ms seit UTC-Mitternacht
//   7..10  Breite  int32, Grad * 1e7
//   11..14 Länge   int32, Grad * 1e7
//   15     Geschwindigkeit, 0.1 m/s (max. 25.5 m/s)
//   16     Kurs, 360/256 Grad
//   17     Bits 7..6 Fix-Art (0 kein Fix, 1 GNSS, 2 DGNSS, 3 geschätzt), Bits 5..0 Satelliten (max. 63)
//   18     HDOP * 10 (max. 25.5)
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace surfcam {

constexpr uint8_t kMhdrProprietary = 0xE0;
constexpr uint8_t kPacketVersion = 1;
constexpr size_t kPacketSize = 19;
constexpr uint32_t kMsPerDay = 86400000UL;

enum FixType : uint8_t {
  FIX_NONE = 0,
  FIX_GNSS = 1,
  FIX_DGNSS = 2,
  FIX_ESTIMATED = 3,
};

struct Position {
  uint8_t deviceId = 0;     // 0..31
  uint8_t seq = 0;
  uint32_t timeMs = 0;      // ms seit UTC-Mitternacht, 0..86'399'999
  int32_t latE7 = 0;
  int32_t lonE7 = 0;
  uint16_t speedCms = 0;    // cm/s
  uint16_t courseCdeg = 0;  // 0..35999 (1/100 Grad)
  uint8_t sats = 0;
  FixType fix = FIX_NONE;
  uint16_t hdopX10 = 0;
  uint16_t batteryMv = 0;
};

// Kodiert p in out. Rückgabe: Anzahl Bytes (kPacketSize) oder 0, wenn out zu klein ist.
size_t encodePosition(const Position& p, uint8_t* out, size_t outLen);

// Dekodiert ein Paket. false bei falscher Länge, falschem MHDR oder falscher Version.
// Werte mit reduzierter Auflösung (Geschwindigkeit, Kurs, HDOP, Akku) werden zurückskaliert.
bool decodePosition(const uint8_t* in, size_t len, Position& p);

// GGA-Fix-Qualität (0..8) in FixType umsetzen.
FixType fixTypeFromGga(uint8_t ggaQuality, bool rmcValid);

// LoRa-Sendezeit in Mikrosekunden (Semtech AN1200.13).
uint32_t loraAirtimeUs(size_t payloadLen, uint8_t sf, uint32_t bwHz, uint8_t crDenom = 5,
                       uint16_t preambleLen = 8, bool crc = true, bool explicitHeader = true);

// Erlaubter Duty Cycle in Promille für eine Mittenfrequenz und Bandbreite (EU 863-870 MHz):
// 100 (10 %) im Band 869.40-869.65 MHz, sonst 10 (1 %).
uint16_t dutyCyclePermille(uint32_t freqHz, uint32_t bwHz);

// Mindestabstand zwischen zwei Sendungen, damit der Duty Cycle eingehalten wird.
uint32_t minTxSpacingMs(uint32_t airtimeUs, uint16_t dutyPermille);

}  // namespace surfcam
