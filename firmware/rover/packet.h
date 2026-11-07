// SurfCam-Funkpaket (LoRa Punkt-zu-Punkt) – reines C++, ohne Arduino-Abhängigkeit.
//
// Aufbau (22 Bytes, Big Endian; 22 statt 23 Bytes, weil 20-22 Bytes gleich viele LoRa-Symbole brauchen):
//   0      MHDR 0xE0 (LoRaWAN "Proprietary"; Netzwerkserver verwerfen solche Frames)
//   1      Bits 7..5 Version (3), Bits 4..0 Geräte-ID (0..31)
//   2      Sequenznummer (0..255, läuft über)
//   3..6   Bits 31..27 Akku-Stufe (0..31 -> 3000 + 40*n mV), Bits 26..0 Zeit des Fixes in ms seit UTC-Mitternacht
//   7..10  Breite  int32, Grad * 1e7
//   11..14 Länge   int32, Grad * 1e7
//   15     Geschwindigkeit, 0.1 m/s (max. 25.5 m/s)
//   16     Kurs, 360/256 Grad
//   17     Bits 7..6 Fix-Art (0 kein Fix, 1 GNSS, 2 DGNSS, 3 geschätzt), Bits 5..0 Satelliten (max. 63)
//   18     HDOP * 10 (max. 25.5)
//   19     Bewegung: RMS der dynamischen Beschleunigung seit dem letzten Paket, 0.05 g (max. 12.7 g)
//   20     Bewegung: Spitzenwert des Beschleunigungsbetrags seit dem letzten Paket, 0.1 g (max. 25.4 g)
//   21     Bits 7..6 Tastenzähler (kurze Drücke, läuft über 3 -> 0; Surf-Pool: Clip speichern)
//          Bits 5..0 Neigung: Winkel zwischen Sensor-Z-Achse und Senkrechter, 3 Grad (0 = flach,
//          60 = 180 Grad kopfüber), 63 = unbekannt
//          19..20: 255 und Neigung 63 = kein Bewegungssensor
//   Version 2 (bis Firmware 0.4.x): Byte 21 = Neigung in Grad ohne Tastenzähler
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace surfcam {

constexpr uint8_t kMhdrProprietary = 0xE0;
constexpr uint8_t kPacketVersion = 3;
constexpr size_t kPacketSize = 22;
constexpr uint8_t kMotionUnknown = 255;
constexpr uint8_t kTiltUnknown = 63;  // Byte 21, Bits 5..0
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
  bool motion = false;      // Bewegungswerte gültig
  uint16_t motionRmsMg = 0;   // RMS dynamische Beschleunigung, mg
  uint16_t motionPeakMg = 0;  // Spitzenwert Beschleunigungsbetrag, mg
  uint8_t tiltDeg = 0;        // 0..180
  uint8_t buttonCount = 0;    // Zähler kurzer Tastendrücke, übertragen werden nur 2 Bits
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

// Erlaubter Duty Cycle in Promille (EU 863-870 MHz, ERC/REC 70-03):
//   869.40-869.65 MHz: 100 (10 %)
//   869.70-870.00 MHz: 1000 (keine Grenze) bei höchstens 7 dBm Sendeleistung (5 mW ERP), sonst 10 (1 %)
//   sonst: 10 (1 %)
uint16_t dutyCyclePermille(uint32_t freqHz, uint32_t bwHz, int8_t txPowerDbm);

// Höchste Sendeleistung (an der Antenne, Antennengewinn ≤ 2 dBi angenommen) für 5 mW ERP
constexpr int8_t kMaxDbmNoDutyBand = 7;

// Mindestabstand zwischen zwei Sendungen, damit der Duty Cycle eingehalten wird.
uint32_t minTxSpacingMs(uint32_t airtimeUs, uint16_t dutyPermille);

}  // namespace surfcam
