#include "tracklog.h"

#include <LittleFS.h>

#include "config.h"
#include "packet.h"

using surfcam::GnssFix;
using surfcam::kMsPerDay;
using surfcam::kTimeUnknown;

char TrackLog::names_[TrackLog::kMaxFiles][32];  // static: nicht auf den Stack des Loop-Tasks

namespace {

constexpr size_t kSessionLen = 13;  // "YYMMDD_hhmmss"

bool isTrackFile(const char* name) {
  size_t l = strlen(name);
  return l > 4 && strcmp(name + l - 4, ".trk") == 0;
}

// "/261106_120000_003.trk" gehört zur Session "261106_120000" (auch alte Dateien ohne Segment)
bool inSession(const char* path, const char* session) {
  return strncmp(path + 1, session, kSessionLen) == 0 && strlen(session) == kSessionLen;
}

// Name ohne führenden '/' und ohne ".trk"
void stripName(const char* in, char* out, size_t outLen) {
  snprintf(out, outLen, "%s", in[0] == '/' ? in + 1 : in);
  size_t l = strlen(out);
  if (l > 4 && strcmp(out + l - 4, ".trk") == 0) out[l - 4] = '\0';
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

uint32_t TrackLog::collect(const char* session) {
  uint32_t n = 0;
  if (!ok_) return 0;
  File root = LittleFS.open("/");
  if (!root) return 0;
  for (File f = root.openNextFile(); f && n < kMaxFiles; f = root.openNextFile()) {
    char path[32];
    normalize(f.name(), path, sizeof(path));
    if (!isTrackFile(path)) continue;
    if (session && !inSession(path, session)) continue;
    // sortiert einfügen (alphabetisch = chronologisch)
    uint32_t i = n++;
    while (i > 0 && strcmp(names_[i - 1], path) > 0) {
      memcpy(names_[i], names_[i - 1], sizeof(names_[0]));
      i--;
    }
    memcpy(names_[i], path, sizeof(names_[0]));
  }
  root.close();
  return n;
}

bool TrackLog::deleteOldest() {
  const uint32_t n = collect(nullptr);
  for (uint32_t i = 0; i < n; i++) {
    if (strcmp(names_[i], name_) == 0) continue;  // aktives Segment nie löschen
    Serial.printf("[log] Platz knapp, lösche %s\r\n", names_[i] + 1);
    return LittleFS.remove(names_[i]);
  }
  return false;
}

bool TrackLog::latestName(char* out, size_t len) {
  if (len == 0) return false;
  const uint32_t n = collect(nullptr);
  if (n == 0) return false;
  char s[32];
  stripName(names_[n - 1], s, sizeof(s));
  s[kSessionLen] = '\0';
  snprintf(out, len, "%s", s);
  return true;
}

bool TrackLog::ensureSpace(size_t needBytes) {
  while (freeBytes() < needBytes + kMinFreeBytes) {
    if (!deleteOldest()) return false;
  }
  return true;
}

bool TrackLog::openSegment() {
  if (!ensureSpace(sizeof(buf_) + sizeof(LogHeader))) {
    if (!full_) Serial.println("[log] Flash voll, Aufzeichnung gestoppt");
    full_ = true;
    return false;
  }
  snprintf(name_, sizeof(name_), "/%s_%03u.trk", session_, (unsigned)segment_);
  file_ = LittleFS.open(name_, FILE_WRITE);
  if (!file_) {
    Serial.printf("[log] kann %s nicht anlegen\r\n", name_ + 1);
    name_[0] = '\0';
    return false;
  }
  LogHeader h;
  memcpy(h.magic, "SCTL", 4);
  h.version = 1;
  h.deviceId = deviceId_;
  h.recordSize = sizeof(LogRecord);
  h.date = date_;
  file_.write((const uint8_t*)&h, sizeof(h));
  file_.flush();
  segBytes_ = sizeof(h);
  return true;
}

bool TrackLog::openSession(const GnssFix& f, uint8_t deviceId) {
  if (f.date == 0 || f.timeMs == kTimeUnknown) return false;
  const uint32_t dd = f.date / 10000, mm = (f.date / 100) % 100, yy = f.date % 100;
  const uint32_t s = f.timeMs / 1000;
  snprintf(session_, sizeof(session_), "%02lu%02lu%02lu_%02lu%02lu%02lu", (unsigned long)yy,
           (unsigned long)mm, (unsigned long)dd, (unsigned long)(s / 3600),
           (unsigned long)((s / 60) % 60), (unsigned long)(s % 60));
  segment_ = 0;
  date_ = f.date;
  deviceId_ = deviceId;
  if (!openSegment()) {
    session_[0] = '\0';
    return false;
  }
  lastTimeMs_ = f.timeMs;
  dayOffsetMs_ = 0;
  records_ = 0;
  haveBucket_ = false;
  lastFlushMs_ = millis();
  Serial.printf("[log] neue Session %s, %u Hz\r\n", session_, rateHz_);
  return true;
}

void TrackLog::append(const GnssFix& f, uint16_t batteryMv, uint8_t deviceId) {
  if (!ok_ || full_ || rateHz_ == 0 || !f.valid || f.timeMs == kTimeUnknown) return;
  if (!file_ && !openSession(f, deviceId)) return;

  // Mitternacht: Zeit springt von ~86'400'000 auf ~0
  if (f.timeMs + kMsPerDay / 2 < lastTimeMs_) dayOffsetMs_ += kMsPerDay;
  lastTimeMs_ = f.timeMs;
  const uint32_t t = f.timeMs + dayOffsetMs_;

  // Lograte: pro Zeitfenster von 1/rate s nur der erste Fix (unabhängig von der GNSS-Rate)
  const uint32_t bucket = t / (1000 / rateHz_);
  if (haveBucket_ && bucket == lastBucket_) return;
  haveBucket_ = true;
  lastBucket_ = bucket;

  LogRecord& r = buf_[count_++];
  r.timeMs = t;
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
  const size_t bytes = count_ * sizeof(LogRecord);
  if (segBytes_ + bytes > kSegmentBytes) {  // nächstes Segment
    file_.close();
    segment_++;
    if (!openSegment()) {
      count_ = 0;
      name_[0] = '\0';
      session_[0] = '\0';
      return;
    }
  }
  if (!ensureSpace(bytes)) {
    Serial.println("[log] Flash voll, Aufzeichnung gestoppt");
    full_ = true;
    count_ = 0;
    close();
    return;
  }
  file_.write((const uint8_t*)buf_, bytes);
  file_.flush();
  segBytes_ += bytes;
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
  session_[0] = '\0';
}

void TrackLog::list(Print& out) {
  if (!ok_) {
    out.println("LittleFS nicht verfügbar");
    return;
  }
  // Pro Session eine Zeile: Segmente, Grösse, Positionen
  const uint32_t n = collect(nullptr);
  uint32_t sessions = 0;
  for (uint32_t i = 0; i < n;) {
    char key[32];
    stripName(names_[i], key, sizeof(key));
    key[kSessionLen] = '\0';
    uint32_t segs = 0;
    size_t size = 0, recs = 0;
    for (; i < n && inSession(names_[i], key); i++) {
      File f = LittleFS.open(names_[i], FILE_READ);
      if (!f) continue;
      const size_t s = f.size();
      f.close();
      size += s;
      if (s > sizeof(LogHeader)) recs += (s - sizeof(LogHeader)) / sizeof(LogRecord);
      segs++;
    }
    out.printf("%s  %3lu Segmente  %8u Bytes  %7u Positionen%s\r\n", key, (unsigned long)segs,
               (unsigned)size, (unsigned)recs, strcmp(key, session_) == 0 ? "  (aktiv)" : "");
    sessions++;
  }
  out.printf("%lu Sessions, %lu Dateien, %u kB frei von %u kB, Lograte %u Hz\r\n",
             (unsigned long)sessions, (unsigned long)n, (unsigned)(freeBytes() / 1024),
             (unsigned)(totalBytes() / 1024), rateHz_);
}

bool TrackLog::dumpFile(const char* path, Print& out, bool header) {
  if (strcmp(path, name_) == 0) flush();  // aktives Segment zuerst schreiben

  File f = LittleFS.open(path, FILE_READ);
  if (!f) return false;
  LogHeader h;
  if (f.read((uint8_t*)&h, sizeof(h)) != sizeof(h) || memcmp(h.magic, "SCTL", 4) != 0 ||
      h.recordSize != sizeof(LogRecord)) {
    f.close();
    return false;
  }
  const uint32_t dd = h.date / 10000, mm = (h.date / 100) % 100, yy = h.date % 100;
  if (header) out.println("date,utc,time_ms,lat,lon,speed_mps,course_deg,sats,fix,hdop,battery_mv");
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

bool TrackLog::dump(const char* name, Print& out) {
  if (!ok_) return false;
  if (isTrackFile(name)) {  // einzelnes Segment
    char path[32];
    normalize(name, path, sizeof(path));
    return dumpFile(path, out, true);
  }
  char key[32];
  stripName(name, key, sizeof(key));
  // Zuerst schreiben: flush() kann über ensureSpace() names_ überschreiben. Während der Ausgabe
  // kommen keine neuen Datensätze dazu, dumpFile() schreibt danach nichts mehr.
  if (strcmp(key, session_) == 0) flush();
  const uint32_t n = collect(key);
  bool any = false;
  for (uint32_t i = 0; i < n; i++) {
    if (dumpFile(names_[i], out, !any)) any = true;
  }
  return any;
}

bool TrackLog::remove(const char* name) {
  if (!ok_) return false;
  full_ = false;
  if (isTrackFile(name)) {
    char path[32];
    normalize(name, path, sizeof(path));
    if (strcmp(path, name_) == 0) close();
    return LittleFS.remove(path);
  }
  char key[32];
  stripName(name, key, sizeof(key));
  if (strcmp(key, session_) == 0) close();
  const uint32_t n = collect(key);
  uint32_t removed = 0;
  for (uint32_t i = 0; i < n; i++) {
    if (LittleFS.remove(names_[i])) removed++;
  }
  return removed > 0;
}

uint32_t TrackLog::removeAll() {
  close();
  full_ = false;
  const uint32_t count = collect(nullptr);  // erst Namen sammeln, dann löschen
  uint32_t n = 0;
  for (uint32_t i = 0; i < count; i++) {
    if (LittleFS.remove(names_[i])) n++;
  }
  return n;
}
