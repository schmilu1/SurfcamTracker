#include "tracklog.h"

#include <LittleFS.h>

#include "config.h"
#include "packet.h"

using surfcam::GnssFix;
using surfcam::kMsPerDay;
using surfcam::kTimeUnknown;

namespace {

bool isTrackFile(const char* name) {
  size_t l = strlen(name);
  return l > 4 && strcmp(name + l - 4, ".trk") == 0;
}

// Grad * 1e7 ohne Fliesskomma-Rundung ausgeben
void printE7(Print& out, int32_t v) {
  int64_t a = v;
  if (a < 0) {
    out.print('-');
    a = -a;
  }
  out.printf("%ld.%07ld", (long)(a / 10000000), (long)(a % 10000000));
}

}  // namespace

bool TrackLog::begin() {
  ok_ = LittleFS.begin(true);  // bei Bedarf formatieren
  if (!ok_) {
    Serial.println("[log] LittleFS nicht verfügbar");
    return false;
  }
  Serial.printf("[log] OK, %u kB frei von %u kB\r\n", (unsigned)(freeBytes() / 1024),
                (unsigned)(totalBytes() / 1024));
  return true;
}

size_t TrackLog::freeBytes() const { return ok_ ? LittleFS.totalBytes() - LittleFS.usedBytes() : 0; }

size_t TrackLog::totalBytes() const { return ok_ ? LittleFS.totalBytes() : 0; }

void TrackLog::normalize(const char* in, char* out, size_t outLen) {
  if (in[0] == '/') {
    snprintf(out, outLen, "%s", in);
  } else {
    snprintf(out, outLen, "/%s", in);
  }
}

bool TrackLog::deleteOldest() {
  File root = LittleFS.open("/");
  if (!root) return false;
  char oldest[32] = "";
  for (File f = root.openNextFile(); f; f = root.openNextFile()) {
    char path[32];
    normalize(f.name(), path, sizeof(path));
    if (!isTrackFile(path) || strcmp(path, name_) == 0) continue;
    // Dateinamen sind Zeitstempel -> alphabetisch kleinster ist der älteste
    if (oldest[0] == '\0' || strcmp(path, oldest) < 0) snprintf(oldest, sizeof(oldest), "%s", path);
  }
  root.close();
  if (oldest[0] == '\0') return false;
  Serial.printf("[log] Platz knapp, lösche %s\r\n", oldest);
  return LittleFS.remove(oldest);
}

bool TrackLog::ensureSpace(size_t needBytes) {
  while (freeBytes() < needBytes + kMinFreeBytes) {
    if (!deleteOldest()) return false;
  }
  return true;
}

bool TrackLog::openSession(const GnssFix& f, uint8_t deviceId) {
  if (f.date == 0 || f.timeMs == kTimeUnknown) return false;
  if (!ensureSpace(sizeof(buf_))) {
    if (!full_) Serial.println("[log] Flash voll, Aufzeichnung gestoppt");
    full_ = true;
    return false;
  }
  const uint32_t dd = f.date / 10000, mm = (f.date / 100) % 100, yy = f.date % 100;
  const uint32_t s = f.timeMs / 1000;
  snprintf(name_, sizeof(name_), "/%02lu%02lu%02lu_%02lu%02lu%02lu.trk", (unsigned long)yy,
           (unsigned long)mm, (unsigned long)dd, (unsigned long)(s / 3600),
           (unsigned long)((s / 60) % 60), (unsigned long)(s % 60));
  file_ = LittleFS.open(name_, FILE_WRITE);
  if (!file_) {
    Serial.printf("[log] kann %s nicht anlegen\r\n", name_);
    name_[0] = '\0';
    return false;
  }
  LogHeader h;
  memcpy(h.magic, "SCTL", 4);
  h.version = 1;
  h.deviceId = deviceId;
  h.recordSize = sizeof(LogRecord);
  h.date = f.date;
  file_.write((const uint8_t*)&h, sizeof(h));
  file_.flush();
  lastTimeMs_ = f.timeMs;
  dayOffsetMs_ = 0;
  records_ = 0;
  lastFlushMs_ = millis();
  Serial.printf("[log] neue Session %s\r\n", name_);
  return true;
}

void TrackLog::append(const GnssFix& f, uint16_t batteryMv, uint8_t deviceId) {
  if (!ok_ || full_ || !f.valid || f.timeMs == kTimeUnknown) return;
  if (!file_ && !openSession(f, deviceId)) return;

  // Mitternacht: Zeit springt von ~86'400'000 auf ~0
  if (f.timeMs + kMsPerDay / 2 < lastTimeMs_) dayOffsetMs_ += kMsPerDay;
  lastTimeMs_ = f.timeMs;

  LogRecord& r = buf_[count_++];
  r.timeMs = f.timeMs + dayOffsetMs_;
  r.latE7 = f.latE7;
  r.lonE7 = f.lonE7;
  r.speedCms = f.speedCms;
  r.courseCdeg = f.courseCdeg;
  r.sats = f.sats;
  r.fixQuality = f.fixQuality;
  r.hdopX10 = f.hdopX10 > 255 ? 255 : (uint8_t)f.hdopX10;
  r.battery = batteryMv > 2000 ? (uint8_t)min(255, (batteryMv - 2000) / 10) : 0;
  records_++;
  if (count_ >= kBufferRecords) flush();
}

void TrackLog::flush() {
  if (!file_ || count_ == 0) return;
  if (!ensureSpace(count_ * sizeof(LogRecord))) {
    Serial.println("[log] Flash voll, Aufzeichnung gestoppt");
    full_ = true;
    count_ = 0;
    close();
    return;
  }
  file_.write((const uint8_t*)buf_, count_ * sizeof(LogRecord));
  file_.flush();
  count_ = 0;
  lastFlushMs_ = millis();
}

void TrackLog::tick() {
  if (file_ && count_ > 0 && millis() - lastFlushMs_ >= timing::LOG_FLUSH_MS) flush();
}

void TrackLog::close() {
  flush();
  if (file_) file_.close();
  name_[0] = '\0';
}

void TrackLog::list(Print& out) {
  if (!ok_) {
    out.println("LittleFS nicht verfügbar");
    return;
  }
  File root = LittleFS.open("/");
  uint32_t n = 0;
  for (File f = root.openNextFile(); f; f = root.openNextFile()) {
    char path[32];
    normalize(f.name(), path, sizeof(path));
    if (!isTrackFile(path)) continue;
    size_t size = f.size();
    size_t recs = size > sizeof(LogHeader) ? (size - sizeof(LogHeader)) / sizeof(LogRecord) : 0;
    out.printf("%s  %7u Bytes  %6u Positionen%s\r\n", path + 1, (unsigned)size, (unsigned)recs,
               strcmp(path, name_) == 0 ? "  (aktiv)" : "");
    n++;
  }
  root.close();
  out.printf("%lu Dateien, %u kB frei von %u kB\r\n", (unsigned long)n,
             (unsigned)(freeBytes() / 1024), (unsigned)(totalBytes() / 1024));
}

bool TrackLog::dump(const char* name, Print& out) {
  char path[32];
  normalize(name, path, sizeof(path));
  if (strcmp(path, name_) == 0) flush();  // aktive Session zuerst schreiben

  File f = LittleFS.open(path, FILE_READ);
  if (!f) return false;
  LogHeader h;
  if (f.read((uint8_t*)&h, sizeof(h)) != sizeof(h) || memcmp(h.magic, "SCTL", 4) != 0 ||
      h.recordSize != sizeof(LogRecord)) {
    f.close();
    return false;
  }
  const uint32_t dd = h.date / 10000, mm = (h.date / 100) % 100, yy = h.date % 100;
  out.println("date,utc,time_ms,lat,lon,speed_mps,course_deg,sats,fix,hdop,battery_mv");
  LogRecord r;
  while (f.read((uint8_t*)&r, sizeof(r)) == sizeof(r)) {
    const uint32_t t = r.timeMs % kMsPerDay;
    out.printf("20%02lu-%02lu-%02lu,%02lu:%02lu:%02lu.%03lu,%lu,", (unsigned long)yy, (unsigned long)mm,
               (unsigned long)dd, (unsigned long)(t / 3600000), (unsigned long)((t / 60000) % 60),
               (unsigned long)((t / 1000) % 60), (unsigned long)(t % 1000), (unsigned long)r.timeMs);
    printE7(out, r.latE7);
    out.print(',');
    printE7(out, r.lonE7);
    out.printf(",%u.%02u,%u.%02u,%u,%u,%u.%u,%u\r\n", r.speedCms / 100, r.speedCms % 100,
               r.courseCdeg / 100, r.courseCdeg % 100, r.sats, r.fixQuality, r.hdopX10 / 10,
               r.hdopX10 % 10, r.battery ? 2000 + r.battery * 10 : 0);
  }
  f.close();
  return true;
}

bool TrackLog::remove(const char* name) {
  char path[32];
  normalize(name, path, sizeof(path));
  if (strcmp(path, name_) == 0) close();
  full_ = false;
  return LittleFS.remove(path);
}

uint32_t TrackLog::removeAll() {
  close();
  full_ = false;
  uint32_t n = 0;
  // Erst Namen sammeln, dann löschen (nicht während der Verzeichnisiteration)
  static char names[64][32];  // static: nicht auf den Stack des Loop-Tasks
  uint32_t count = 0;
  File root = LittleFS.open("/");
  for (File f = root.openNextFile(); f && count < 64; f = root.openNextFile()) {
    normalize(f.name(), names[count], sizeof(names[0]));
    if (isTrackFile(names[count])) count++;
  }
  root.close();
  for (uint32_t i = 0; i < count; i++) {
    if (LittleFS.remove(names[i])) n++;
  }
  return n;
}
