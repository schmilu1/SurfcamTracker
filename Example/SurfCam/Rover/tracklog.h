// Positionslog im internen Flash (LittleFS, Partition "spiffs").
//
// Pro Session (ab dem ersten gültigen Fix) eine Datei "/YYMMDD_hhmmss.trk":
//   Kopf  12 Bytes: "SCTL", Version (1), Geräte-ID, Datensatzgrösse (uint16), Datum DDMMYY (uint32)
//   Datensätze à 20 Bytes, Little Endian (siehe LogRecord)
// Wird der Platz knapp, wird die älteste Session gelöscht.
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

  // Gültige Fixes anhängen; startet beim ersten Fix eine neue Session-Datei.
  void append(const surfcam::GnssFix& f, uint16_t batteryMv, uint8_t deviceId);
  void tick();   // regelmässig aufrufen (zeitgesteuertes Schreiben)
  void close();  // Puffer schreiben und Datei schliessen

  void list(Print& out);
  bool dump(const char* name, Print& out);  // als CSV
  bool remove(const char* name);
  uint32_t removeAll();

  size_t freeBytes() const;
  size_t totalBytes() const;
  const char* currentName() const { return name_; }
  uint32_t recordsInSession() const { return records_; }

 private:
  bool openSession(const surfcam::GnssFix& f, uint8_t deviceId);
  void flush();
  bool ensureSpace(size_t needBytes);
  bool deleteOldest();
  static void normalize(const char* in, char* out, size_t outLen);

  static constexpr size_t kBufferRecords = 64;
  static constexpr size_t kMinFreeBytes = 32 * 1024;

  bool ok_ = false;
  bool full_ = false;
  File file_;
  char name_[32] = "";
  uint32_t lastTimeMs_ = 0;
  uint32_t dayOffsetMs_ = 0;
  LogRecord buf_[kBufferRecords];
  size_t count_ = 0;
  uint32_t lastFlushMs_ = 0;
  uint32_t records_ = 0;
};
