#include "nmea.h"

#include <string.h>

namespace surfcam {

namespace {

int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

int splitFields(char* s, char** fields, int maxFields) {
  int n = 0;
  fields[n++] = s;
  for (char* p = s; *p; p++) {
    if (*p == ',') {
      *p = '\0';
      if (n >= maxFields) break;
      fields[n++] = p + 1;
    }
  }
  return n;
}

// Satztyp ohne Talker-ID, z. B. "GNRMC" -> "RMC"
const char* sentenceType(const char* id) {
  size_t l = strlen(id);
  return l >= 3 ? id + (l - 3) : id;
}

}  // namespace

void NmeaParser::reset() {
  len_ = 0;
  inSentence_ = false;
  overflow_ = false;
  cur_ = GnssFix();
  out_ = GnssFix();
  haveLastGga_ = false;
  lastGgaTimeMs_ = kTimeUnknown;
}

uint8_t NmeaParser::checksum(const char* body) {
  uint8_t c = 0;
  while (*body) c ^= (uint8_t)*body++;
  return c;
}

bool NmeaParser::parseFixed(const char* s, uint8_t decimals, int64_t& out) {
  if (!s || !*s) return false;
  bool neg = false;
  if (*s == '-' || *s == '+') {
    neg = (*s == '-');
    s++;
  }
  int64_t intPart = 0;
  int digits = 0;
  while (*s >= '0' && *s <= '9') {
    intPart = intPart * 10 + (*s - '0');
    s++;
    digits++;
  }
  int64_t frac = 0;
  uint8_t fracDigits = 0;
  if (*s == '.') {
    s++;
    while (*s >= '0' && *s <= '9') {
      if (fracDigits < decimals) {
        frac = frac * 10 + (*s - '0');
        fracDigits++;
      }
      s++;
      digits++;
    }
  }
  if (*s != '\0' || digits == 0) return false;
  while (fracDigits < decimals) {
    frac *= 10;
    fracDigits++;
  }
  int64_t scale = 1;
  for (uint8_t i = 0; i < decimals; i++) scale *= 10;
  int64_t v = intPart * scale + frac;
  out = neg ? -v : v;
  return true;
}

bool NmeaParser::parseTimeMs(const char* s, uint32_t& out) {
  if (!s || strlen(s) < 6) return false;
  for (int i = 0; i < 6; i++) {
    if (s[i] < '0' || s[i] > '9') return false;
  }
  uint32_t hh = (s[0] - '0') * 10 + (s[1] - '0');
  uint32_t mm = (s[2] - '0') * 10 + (s[3] - '0');
  int64_t ssMs = 0;
  if (!parseFixed(s + 4, 3, ssMs)) return false;
  if (hh > 23 || mm > 59 || ssMs >= 61000) return false;
  out = hh * 3600000UL + mm * 60000UL + (uint32_t)ssMs;
  return true;
}

bool NmeaParser::parseCoordE7(const char* s, char hemi, int32_t& out) {
  int64_t v = 0;  // ddmm.mmmmmmm * 1e7
  if (!parseFixed(s, 7, v) || v < 0) return false;
  int64_t deg = v / 1000000000LL;                 // dd (bzw. ddd)
  int64_t minE7 = v - deg * 1000000000LL;         // mm.mmmmmmm * 1e7
  if (minE7 >= 600000000LL) return false;         // Minuten >= 60
  int64_t e7 = deg * 10000000LL + (minE7 + 30) / 60;
  if (hemi == 'S' || hemi == 'W') e7 = -e7;
  else if (hemi != 'N' && hemi != 'E') return false;
  out = (int32_t)e7;
  return true;
}

bool NmeaParser::feed(char c) {
  if (c == '$') {
    len_ = 0;
    inSentence_ = true;
    overflow_ = false;
    buf_[len_++] = c;
    return false;
  }
  if (!inSentence_) return false;
  if (c == '\r' || c == '\n') {
    inSentence_ = false;
    if (overflow_) {
      stats_.overflows++;
      return false;
    }
    buf_[len_] = '\0';
    return processLine();
  }
  if (len_ < kMaxLine) {
    buf_[len_++] = c;
  } else {
    overflow_ = true;
  }
  return false;
}

bool NmeaParser::processLine() {
  // buf_ = "$GNRMC,...*hh"
  char* star = strrchr(buf_, '*');
  if (!star || star[1] == '\0' || star[2] == '\0') {
    stats_.checksumErrors++;
    return false;
  }
  int h = hexVal(star[1]), l = hexVal(star[2]);
  *star = '\0';
  if (h < 0 || l < 0 || checksum(buf_ + 1) != (uint8_t)(h * 16 + l)) {
    stats_.checksumErrors++;
    return false;
  }
  stats_.sentences++;

  char* fields[kMaxFields];
  int n = splitFields(buf_ + 1, fields, kMaxFields);
  const char* type = sentenceType(fields[0]);
  if (strcmp(type, "RMC") == 0) return handleRmc(fields, n);
  if (strcmp(type, "GGA") == 0) return handleGga(fields, n);
  return false;
}

bool NmeaParser::beginEpoch(uint32_t timeMs) {
  bool emitted = false;
  if ((cur_.hasRmc || cur_.hasGga) && cur_.timeMs != timeMs) {
    // Neue Epoche beginnt; alte nur melden, wenn sie einen RMC hatte
    if (cur_.hasRmc) emitted = emit();
    cur_ = GnssFix();
  }
  cur_.timeMs = timeMs;
  return emitted;
}

bool NmeaParser::emit() {
  out_ = cur_;
  if (!out_.hasRmc) out_.valid = out_.fixQuality > 0;
  if (out_.hasRmc && !out_.hasGga) {
    stats_.rmcOnly++;
    // GGA verloren: Werte des letzten GGA übernehmen, wenn er höchstens 2 s alt ist
    if (haveLastGga_ && out_.timeMs != kTimeUnknown && lastGgaTimeMs_ != kTimeUnknown) {
      const uint32_t day = 86400000UL;
      const uint32_t age = (out_.timeMs + day - lastGgaTimeMs_) % day;
      if (age <= 2000) {
        out_.fixQuality = lastFixQuality_;
        out_.sats = lastSats_;
        out_.hdopX10 = lastHdopX10_;
        out_.altDm = lastAltDm_;
        out_.ggaCarried = true;
      }
    }
  }
  stats_.epochs++;
  cur_ = GnssFix();
  return true;
}

// $xxRMC,hhmmss.ss,A,ddmm.mmmm,N,dddmm.mmmm,E,speed_kn,course,ddmmyy,...
bool NmeaParser::handleRmc(char** f, int n) {
  if (n < 10) return false;
  stats_.rmc++;
  uint32_t t = kTimeUnknown;
  parseTimeMs(f[1], t);
  bool emitted = beginEpoch(t);

  cur_.hasRmc = true;
  cur_.valid = (f[2][0] == 'A');
  int32_t v;
  if (parseCoordE7(f[3], f[4][0], v)) cur_.latE7 = v;
  if (parseCoordE7(f[5], f[6][0], v)) cur_.lonE7 = v;
  int64_t x;
  if (parseFixed(f[7], 3, x) && x >= 0) {
    // Knoten * 1000 -> cm/s (1 kn = 51.4444 cm/s)
    int64_t cms = (x * 514444 + 5000000) / 10000000;
    cur_.speedCms = cms > 65535 ? 65535 : (uint16_t)cms;
  }
  if (parseFixed(f[8], 2, x) && x >= 0) cur_.courseCdeg = (uint16_t)(x % 36000);
  if (parseFixed(f[9], 0, x) && x > 0) cur_.date = (uint32_t)x;

  if (cur_.hasGga) return emit() || emitted;
  return emitted;
}

// $xxGGA,hhmmss.ss,ddmm.mmmm,N,dddmm.mmmm,E,q,sats,hdop,alt,M,...
bool NmeaParser::handleGga(char** f, int n) {
  if (n < 10) return false;
  stats_.gga++;
  uint32_t t = kTimeUnknown;
  parseTimeMs(f[1], t);
  bool emitted = beginEpoch(t);

  cur_.hasGga = true;
  int64_t x;
  cur_.fixQuality = parseFixed(f[6], 0, x) ? (uint8_t)x : 0;
  cur_.sats = parseFixed(f[7], 0, x) ? (uint8_t)(x > 255 ? 255 : x) : 0;
  if (parseFixed(f[8], 1, x) && x >= 0) cur_.hdopX10 = x > 65535 ? 65535 : (uint16_t)x;
  if (parseFixed(f[9], 1, x)) cur_.altDm = (int32_t)x;
  haveLastGga_ = true;
  lastGgaTimeMs_ = t;
  lastFixQuality_ = cur_.fixQuality;
  lastSats_ = cur_.sats;
  lastHdopX10_ = cur_.hdopX10;
  lastAltDm_ = cur_.altDm;
  if (!cur_.hasRmc) {
    int32_t v;
    if (parseCoordE7(f[2], f[3][0], v)) cur_.latE7 = v;
    if (parseCoordE7(f[4], f[5][0], v)) cur_.lonE7 = v;
  }

  if (cur_.hasRmc) return emit() || emitted;
  return emitted;
}

}  // namespace surfcam
