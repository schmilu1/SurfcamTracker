// Positionslog im internen Flash (LittleFS, Partition "spiffs").
//
// Eine Session beginnt mit dem ersten gültigen Fix und heisst nach dessen UTC-Zeit "YYMMDD_hhmmss".
// Sie besteht aus Segmenten à höchstens 64 kB: "/YYMMDD_hhmmss_NNN.trk" (NNN = 000, 001, ...).
// Jedes Segment ist eine eigenständige Datei:
//   Kopf  12 Bytes: "SCTL", Version (1), Geräte-ID, Datensatzgrösse (uint16), Datum DDMMYY (uint32)
//   Datensätze à 20 Bytes, Little Endian (siehe LogRecord)
// Ringpuffer: Wird der Platz knapp, wird das älteste Segment gelöscht, auch eines der laufenden
// Session. Die Dateinamen sortieren alphabetisch = chronologisch.
#pragma once

#include <Arduino.h>
#include <FS.h>

#include "nmea.h"

struct __attribute__((packed)) LogHeader {
  char magic[4];        // "SCTL"
  uint8_t version;      // 1
  uint8_t deviceId;
  uint16_t recordSize;  // 20
  uint32_t date;        // DDMMYY des ersten Fixes (UTC)
};

struct __attribute__((packed)) LogRecord {
  uint32_t timeMs;      // ms seit UTC-Mitternacht des Kopfdatums (läuft über Mitternacht weiter)
  int32_t latE7;
  int32_t lonE7;
  uint16_t speedCms;
  uint16_t courseCdeg;
  uint8_t sats;
  uint8_t fixQuality;   // GGA-Qualität
  uint8_t hdopX10;      // max. 255
  uint8_t battery;      // (mV - 2000) / 10, 0 = unbekannt
};

static_assert(sizeof(LogHeader) == 12, "LogHeader muss 12 Bytes haben");
static_assert(sizeof(LogRecord) == 20, "LogRecord muss 20 Bytes haben");

class TrackLog {
 public:
  bool begin();
  bool ok() const { return ok_; }

  // Lograte in Hz (1, 2, 4, 5, 10; 0 = aus). Ausgewählt wird nach UTC-Zeit des Fixes.
  void setRate(uint8_t hz) { rateHz_ = hz; }
  uint8_t rate() const { return rateHz_; }

  // Gültige Fixes anhängen; startet beim ersten Fix eine neue Session.
  void append(const surfcam::GnssFix& f, uint16_t batteryMv, uint8_t deviceId);
  void tick();   // regelmässig aufrufen (zeitgesteuertes Schreiben)
  void close();  // Puffer schreiben und Datei schliessen

  // name: Session "YYMMDD_hhmmss" (alle Segmente) oder eine einzelne Datei "....trk"
  void list(Print& out);
  bool dump(const char* name, Print& out);  // als CSV
  bool latestName(char* out, size_t len);   // neueste Session
  bool remove(const char* name);
  uint32_t removeAll();

  size_t freeBytes() const;
  size_t totalBytes() const;
  const char* currentName() const { return session_; }  // aktive Session oder ""

  uint32_t recordsInSession() const { return records_; }

 private:
  bool openSession(const surfcam::GnssFix& f, uint8_t deviceId);
  bool openSegment();
  void flush();
  bool dumpFile(const char* path, Print& out, bool header);
  uint32_t collect(const char* session);  // Track-Dateien (optional nur einer Session) sortiert in names_
  bool ensureSpace(size_t needBytes);
  bool deleteOldest();
  static void normalize(const char* in, char* out, size_t outLen);

  static constexpr size_t kBufferRecords = 64;
  static constexpr size_t kMinFreeBytes = 32 * 1024;
  static constexpr size_t kSegmentBytes = 64 * 1024;
  static constexpr size_t kMaxFiles = 160;

  bool ok_ = false;
  bool full_ = false;
  File file_;
  char name_[40] = "";     // aktives Segment, z. B. "/261106_120000_003.trk"
  char session_[24] = "";  // aktive Session, z. B. "261106_120000"
  uint16_t segment_ = 0;
  uint32_t date_ = 0;
  uint8_t deviceId_ = 0;
  size_t segBytes_ = 0;
  uint8_t rateHz_ = 5;
  bool haveBucket_ = false;
  uint32_t lastBucket_ = 0;
  static char names_[kMaxFiles][32];
  uint32_t lastTimeMs_ = 0;
  uint32_t dayOffsetMs_ = 0;
  LogRecord buf_[kBufferRecords];
  size_t count_ = 0;
  uint32_t lastFlushMs_ = 0;
  uint32_t records_ = 0;
};
