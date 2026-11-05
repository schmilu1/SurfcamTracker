// Unit-Tests für den NMEA-Parser. Ausführen: pio test -e native
#include <stdio.h>
#include <string.h>
#include <unity.h>

#include "nmea.h"

using namespace surfcam;

// Bekannte Beispielsätze (mit originaler Prüfsumme)
static const char* kRmc = "$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A\r\n";
static const char* kGga = "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47\r\n";

static int feedAll(NmeaParser& p, const char* s) {
  int epochs = 0;
  while (*s) {
    if (p.feed(*s++)) epochs++;
  }
  return epochs;
}

// Baut "$<body>*hh\r\n" mit korrekter Prüfsumme
static void sentence(char* out, size_t len, const char* body) {
  snprintf(out, len, "$%s*%02X\r\n", body, NmeaParser::checksum(body));
}

void setUp() {}
void tearDown() {}

void test_checksum_of_known_sentence() {
  TEST_ASSERT_EQUAL_HEX8(0x6A, NmeaParser::checksum(
      "GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W"));
}

void test_parse_fixed() {
  int64_t v;
  TEST_ASSERT_TRUE(NmeaParser::parseFixed("12.345", 3, v));
  TEST_ASSERT_EQUAL_INT64(12345, v);
  TEST_ASSERT_TRUE(NmeaParser::parseFixed("12.3", 3, v));
  TEST_ASSERT_EQUAL_INT64(12300, v);
  TEST_ASSERT_TRUE(NmeaParser::parseFixed("12.34567", 2, v));  // abgeschnitten
  TEST_ASSERT_EQUAL_INT64(1234, v);
  TEST_ASSERT_TRUE(NmeaParser::parseFixed("-5.5", 1, v));
  TEST_ASSERT_EQUAL_INT64(-55, v);
  TEST_ASSERT_TRUE(NmeaParser::parseFixed("7", 0, v));
  TEST_ASSERT_EQUAL_INT64(7, v);
  TEST_ASSERT_FALSE(NmeaParser::parseFixed("", 2, v));
  TEST_ASSERT_FALSE(NmeaParser::parseFixed("1.2x", 2, v));
  TEST_ASSERT_FALSE(NmeaParser::parseFixed(".", 2, v));
}

void test_parse_time() {
  uint32_t t;
  TEST_ASSERT_TRUE(NmeaParser::parseTimeMs("123519", t));
  TEST_ASSERT_EQUAL_UINT32(45319000UL, t);
  TEST_ASSERT_TRUE(NmeaParser::parseTimeMs("235959.80", t));
  TEST_ASSERT_EQUAL_UINT32(86399800UL, t);
  TEST_ASSERT_TRUE(NmeaParser::parseTimeMs("000000.200", t));
  TEST_ASSERT_EQUAL_UINT32(200UL, t);
  TEST_ASSERT_FALSE(NmeaParser::parseTimeMs("", t));
  TEST_ASSERT_FALSE(NmeaParser::parseTimeMs("12a519", t));
  TEST_ASSERT_FALSE(NmeaParser::parseTimeMs("246000", t));
}

void test_parse_coord() {
  int32_t v;
  TEST_ASSERT_TRUE(NmeaParser::parseCoordE7("4807.038", 'N', v));
  TEST_ASSERT_EQUAL_INT32(481173000, v);
  TEST_ASSERT_TRUE(NmeaParser::parseCoordE7("01131.000", 'E', v));
  TEST_ASSERT_EQUAL_INT32(115166667, v);
  TEST_ASSERT_TRUE(NmeaParser::parseCoordE7("3351.1234567", 'S', v));
  TEST_ASSERT_EQUAL_INT32(-338520576, v);  // -(33 + 51.1234567/60)
  TEST_ASSERT_TRUE(NmeaParser::parseCoordE7("15112.5", 'W', v));
  TEST_ASSERT_EQUAL_INT32(-1512083333, v);
  TEST_ASSERT_FALSE(NmeaParser::parseCoordE7("", 'N', v));
  TEST_ASSERT_FALSE(NmeaParser::parseCoordE7("4807.038", 'X', v));
  TEST_ASSERT_FALSE(NmeaParser::parseCoordE7("4867.000", 'N', v));  // 67 Minuten
}

void test_rmc_then_gga_gives_one_epoch() {
  NmeaParser p;
  TEST_ASSERT_EQUAL(0, feedAll(p, kRmc));
  TEST_ASSERT_EQUAL(1, feedAll(p, kGga));
  const GnssFix& f = p.epoch();
  TEST_ASSERT_TRUE(f.valid);
  TEST_ASSERT_TRUE(f.hasRmc);
  TEST_ASSERT_TRUE(f.hasGga);
  TEST_ASSERT_EQUAL_UINT32(45319000UL, f.timeMs);
  TEST_ASSERT_EQUAL_UINT32(230394UL, f.date);
  TEST_ASSERT_EQUAL_INT32(481173000, f.latE7);
  TEST_ASSERT_EQUAL_INT32(115166667, f.lonE7);
  TEST_ASSERT_EQUAL_UINT16(1152, f.speedCms);  // 22.4 kn = 11.52 m/s
  TEST_ASSERT_EQUAL_UINT16(8440, f.courseCdeg);
  TEST_ASSERT_EQUAL_UINT8(1, f.fixQuality);
  TEST_ASSERT_EQUAL_UINT8(8, f.sats);
  TEST_ASSERT_EQUAL_UINT16(9, f.hdopX10);
  TEST_ASSERT_EQUAL_INT32(5454, f.altDm);
  TEST_ASSERT_EQUAL_UINT32(1, p.stats().epochs);
}

void test_gga_then_rmc_gives_one_epoch() {
  NmeaParser p;
  TEST_ASSERT_EQUAL(0, feedAll(p, kGga));
  TEST_ASSERT_EQUAL(1, feedAll(p, kRmc));
  TEST_ASSERT_EQUAL_UINT8(8, p.epoch().sats);
  TEST_ASSERT_EQUAL_UINT16(1152, p.epoch().speedCms);
}

void test_rmc_only_emits_on_next_epoch() {
  NmeaParser p;
  char s[128];
  sentence(s, sizeof(s), "GNRMC,100000.00,A,4700.0000,N,00800.0000,E,1.0,90.0,051126,,,A");
  TEST_ASSERT_EQUAL(0, feedAll(p, s));
  sentence(s, sizeof(s), "GNRMC,100000.20,A,4700.0001,N,00800.0000,E,1.0,90.0,051126,,,A");
  TEST_ASSERT_EQUAL(1, feedAll(p, s));
  TEST_ASSERT_EQUAL_UINT32(36000000UL, p.epoch().timeMs);
  TEST_ASSERT_FALSE(p.epoch().hasGga);
  TEST_ASSERT_TRUE(p.epoch().valid);
}

void test_five_hz_stream() {
  NmeaParser p;
  char s[128];
  int epochs = 0;
  for (int i = 0; i < 10; i++) {
    char body[100];
    int cs = i * 20;  // 200 ms
    snprintf(body, sizeof(body), "GNGGA,1200%02d.%02d,4700.0000,N,00800.0000,E,1,12,0.7,3.0,M,48.0,M,,",
             cs / 100, cs % 100);
    sentence(s, sizeof(s), body);
    epochs += feedAll(p, s);
    snprintf(body, sizeof(body), "GNRMC,1200%02d.%02d,A,4700.0000,N,00800.0000,E,0.5,10.0,051126,,,A",
             cs / 100, cs % 100);
    sentence(s, sizeof(s), body);
    epochs += feedAll(p, s);
  }
  TEST_ASSERT_EQUAL(10, epochs);
  TEST_ASSERT_EQUAL_UINT32(43201800UL, p.epoch().timeMs);  // 12:00:01.80
}

void test_no_fix_epoch() {
  NmeaParser p;
  char s[128];
  sentence(s, sizeof(s), "GNGGA,,,,,,0,00,25.5,,,,,,");
  feedAll(p, s);
  sentence(s, sizeof(s), "GNRMC,,V,,,,,,,,,,N");
  TEST_ASSERT_EQUAL(1, feedAll(p, s));
  TEST_ASSERT_FALSE(p.epoch().valid);
  TEST_ASSERT_EQUAL_UINT32(kTimeUnknown, p.epoch().timeMs);
  TEST_ASSERT_EQUAL_UINT8(0, p.epoch().sats);
}

void test_southern_western_hemisphere() {
  NmeaParser p;
  char s[128];
  sentence(s, sizeof(s), "GPRMC,081836,A,3751.65,S,14507.36,W,000.0,360.0,130998,011.3,E");
  feedAll(p, s);
  sentence(s, sizeof(s), "GPGGA,081836,3751.65,S,14507.36,W,1,05,1.5,10.0,M,0.0,M,,");
  TEST_ASSERT_EQUAL(1, feedAll(p, s));
  TEST_ASSERT_TRUE(p.epoch().latE7 < 0);
  TEST_ASSERT_TRUE(p.epoch().lonE7 < 0);
  TEST_ASSERT_EQUAL_INT32(-378608333, p.epoch().latE7);
  TEST_ASSERT_EQUAL_INT32(-1451226667, p.epoch().lonE7);
}

void test_bad_checksum_is_rejected() {
  NmeaParser p;
  const char* bad = "$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6B\r\n";
  TEST_ASSERT_EQUAL(0, feedAll(p, bad));
  TEST_ASSERT_EQUAL(0, feedAll(p, kGga));
  TEST_ASSERT_EQUAL_UINT32(1, p.stats().checksumErrors);
  TEST_ASSERT_EQUAL_UINT32(0, p.stats().epochs);
}

void test_missing_checksum_is_rejected() {
  NmeaParser p;
  TEST_ASSERT_EQUAL(0, feedAll(p, "$GPRMC,123519,A,4807.038,N\r\n"));
  TEST_ASSERT_EQUAL_UINT32(1, p.stats().checksumErrors);
}

void test_overlong_line_is_ignored() {
  NmeaParser p;
  char s[300];
  memset(s, 'A', sizeof(s));
  s[0] = '$';
  s[297] = '\r';
  s[298] = '\n';
  s[299] = '\0';
  TEST_ASSERT_EQUAL(0, feedAll(p, s));
  TEST_ASSERT_EQUAL_UINT32(1, p.stats().overflows);
  // danach funktioniert der Parser weiter
  feedAll(p, kRmc);
  TEST_ASSERT_EQUAL(1, feedAll(p, kGga));
}

void test_garbage_between_sentences() {
  NmeaParser p;
  feedAll(p, "xx\x01\xff");
  feedAll(p, kRmc);
  feedAll(p, "noise$noise\r\n");  // abgebrochener Satz
  TEST_ASSERT_EQUAL(1, feedAll(p, kGga));
}

void test_other_sentences_are_ignored() {
  NmeaParser p;
  char s[128];
  sentence(s, sizeof(s), "GPTXT,01,01,02,SW=URANUS5,V5.3.0.0");
  TEST_ASSERT_EQUAL(0, feedAll(p, s));
  TEST_ASSERT_EQUAL_UINT32(1, p.stats().sentences);
  TEST_ASSERT_EQUAL_UINT32(0, p.stats().rmc);
}

// GGA bzw. RMC um 12:00:<sec>.<cs> (sec 0..59, cs Hundertstel)
static int feedGga(NmeaParser& p, int sec, int cs) {
  char s[128], body[100];
  snprintf(body, sizeof(body), "GNGGA,1200%02d.%02d,4700.0000,N,00800.0000,E,1,11,1.2,412.5,M,47.1,M,,",
           sec, cs);
  sentence(s, sizeof(s), body);
  return feedAll(p, s);
}

static int feedRmc(NmeaParser& p, int sec, int cs) {
  char s[128], body[100];
  snprintf(body, sizeof(body), "GNRMC,1200%02d.%02d,A,4700.0000,N,00800.0000,E,0.0,0.0,051126,,,A",
           sec, cs);
  sentence(s, sizeof(s), body);
  return feedAll(p, s);
}

void test_missing_gga_carries_previous_values() {
  NmeaParser p;
  feedGga(p, 0, 0);
  TEST_ASSERT_EQUAL(1, feedRmc(p, 0, 0));   // 12:00:00.00 vollständig
  TEST_ASSERT_EQUAL(0, feedRmc(p, 0, 20));  // 12:00:00.20 nur RMC -> wartet
  TEST_ASSERT_EQUAL(1, feedGga(p, 0, 40));  // nächster GGA meldet die Epoche ohne GGA
  const GnssFix& f = p.epoch();
  TEST_ASSERT_EQUAL_UINT32(43200200UL, f.timeMs);
  TEST_ASSERT_FALSE(f.hasGga);
  TEST_ASSERT_TRUE(f.ggaCarried);
  TEST_ASSERT_TRUE(f.valid);
  TEST_ASSERT_EQUAL_UINT8(1, f.fixQuality);
  TEST_ASSERT_EQUAL_UINT8(11, f.sats);
  TEST_ASSERT_EQUAL_UINT16(12, f.hdopX10);
  TEST_ASSERT_EQUAL_INT32(4125, f.altDm);
  TEST_ASSERT_EQUAL_UINT32(1, p.stats().rmcOnly);
  // danach wieder normale Epochen ohne Übernahme
  TEST_ASSERT_EQUAL(1, feedRmc(p, 0, 40));
  TEST_ASSERT_FALSE(p.epoch().ggaCarried);
}

void test_missing_gga_older_than_2s_is_not_carried() {
  NmeaParser p;
  feedGga(p, 0, 0);
  TEST_ASSERT_EQUAL(1, feedRmc(p, 0, 0));
  TEST_ASSERT_EQUAL(0, feedRmc(p, 3, 0));   // 3 s später, GGA fehlt
  TEST_ASSERT_EQUAL(1, feedGga(p, 3, 20));
  TEST_ASSERT_FALSE(p.epoch().ggaCarried);
  TEST_ASSERT_EQUAL_UINT8(0, p.epoch().sats);
  TEST_ASSERT_TRUE(p.epoch().valid);  // RMC sagt weiterhin 'A'
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_checksum_of_known_sentence);
  RUN_TEST(test_parse_fixed);
  RUN_TEST(test_parse_time);
  RUN_TEST(test_parse_coord);
  RUN_TEST(test_rmc_then_gga_gives_one_epoch);
  RUN_TEST(test_gga_then_rmc_gives_one_epoch);
  RUN_TEST(test_rmc_only_emits_on_next_epoch);
  RUN_TEST(test_five_hz_stream);
  RUN_TEST(test_no_fix_epoch);
  RUN_TEST(test_southern_western_hemisphere);
  RUN_TEST(test_bad_checksum_is_rejected);
  RUN_TEST(test_missing_checksum_is_rejected);
  RUN_TEST(test_overlong_line_is_ignored);
  RUN_TEST(test_garbage_between_sentences);
  RUN_TEST(test_other_sentences_are_ignored);
  RUN_TEST(test_missing_gga_carries_previous_values);
  RUN_TEST(test_missing_gga_older_than_2s_is_not_carried);
  return UNITY_END();
}
