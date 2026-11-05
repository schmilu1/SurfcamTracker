// NMEA-Parser für RMC und GGA (beliebige Talker-ID: GP, GN, BD, GL, ...).
// Reines C++ ohne Arduino und ohne Fliesskomma, damit er nativ getestet werden kann.
//
// RMC und GGA mit gleichem Zeitstempel werden zu einer Epoche zusammengefasst.
// Eine Epoche wird gemeldet, sobald beide Sätze da sind oder sobald ein Satz mit
// neuem Zeitstempel beginnt und die alte Epoche zumindest einen RMC hatte.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace surfcam {

constexpr uint32_t kTimeUnknown = 0xFFFFFFFFUL;

struct GnssFix {
  uint32_t date = 0;            // DDMMYY als Zahl (z. B. 51126 für 05.11.26), 0 = unbekannt
  uint32_t timeMs = kTimeUnknown;  // ms seit UTC-Mitternacht
  bool valid = false;           // Position gültig (RMC 'A' bzw. GGA-Qualität > 0)
  int32_t latE7 = 0;
  int32_t lonE7 = 0;
  uint16_t speedCms = 0;
  uint16_t courseCdeg = 0;
  uint8_t fixQuality = 0;       // GGA: 0 kein Fix, 1 GPS, 2 DGPS, 6 geschätzt, ...
  uint8_t sats = 0;
  uint16_t hdopX10 = 0;
  int32_t altDm = 0;            // Höhe über Meer in Dezimetern
  bool hasRmc = false;
  bool hasGga = false;
};

struct NmeaStats {
  uint32_t sentences = 0;       // gültige Sätze (Prüfsumme ok), alle Typen
  uint32_t checksumErrors = 0;
  uint32_t overflows = 0;       // zu lange Zeilen
  uint32_t rmc = 0;
  uint32_t gga = 0;
  uint32_t epochs = 0;
};

class NmeaParser {
 public:
  // Ein Zeichen verarbeiten. true, wenn dadurch eine Epoche fertig wurde (siehe epoch()).
  bool feed(char c);

  const GnssFix& epoch() const { return out_; }
  const NmeaStats& stats() const { return stats_; }
  void resetStats() { stats_ = NmeaStats(); }
  void reset();

  // Hilfsfunktionen (öffentlich für Tests)
  static uint8_t checksum(const char* body);
  // Dezimalzahl mit optionalem Vorzeichen -> Ganzzahl mit 'decimals' Nachkommastellen
  // (weitere Stellen werden abgeschnitten). false bei leerem oder ungültigem Text.
  static bool parseFixed(const char* s, uint8_t decimals, int64_t& out);
  // "hhmmss" oder "hhmmss.sss" -> ms seit Mitternacht
  static bool parseTimeMs(const char* s, uint32_t& out);
  // "ddmm.mmmm" / "dddmm.mmmm" + Hemisphäre -> Grad * 1e7
  static bool parseCoordE7(const char* s, char hemi, int32_t& out);

 private:
  static constexpr size_t kMaxLine = 100;
  static constexpr int kMaxFields = 20;

  bool processLine();
  bool beginEpoch(uint32_t timeMs);  // true, wenn dabei die alte Epoche gemeldet wurde
  bool emit();
  bool handleRmc(char** f, int n);
  bool handleGga(char** f, int n);

  char buf_[kMaxLine + 1];
  size_t len_ = 0;
  bool inSentence_ = false;
  bool overflow_ = false;
  GnssFix cur_;
  GnssFix out_;
  NmeaStats stats_;
};

}  // namespace surfcam
