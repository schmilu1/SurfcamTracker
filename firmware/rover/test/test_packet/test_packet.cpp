// Unit-Tests für das Funkpaket und die Sendezeit-Berechnung. Ausführen: pio test -e native
#include <unity.h>

#include "packet.h"

using namespace surfcam;

void setUp() {}
void tearDown() {}

static Position sample() {
  Position p;
  p.deviceId = 7;
  p.seq = 200;
  p.timeMs = 45319200;  // 12:35:19.200
  p.latE7 = 471234567;
  p.lonE7 = -84567890;
  p.speedCms = 734;      // 7.34 m/s
  p.courseCdeg = 27050;  // 270.50°
  p.sats = 17;
  p.fix = FIX_GNSS;
  p.hdopX10 = 8;
  p.batteryMv = 3912;
  return p;
}

void test_size_and_header() {
  uint8_t buf[32];
  TEST_ASSERT_EQUAL(kPacketSize, encodePosition(sample(), buf, sizeof(buf)));
  TEST_ASSERT_EQUAL(19, kPacketSize);
  TEST_ASSERT_EQUAL_HEX8(0xE0, buf[0]);
  TEST_ASSERT_EQUAL_HEX8((1 << 5) | 7, buf[1]);
  TEST_ASSERT_EQUAL_UINT8(200, buf[2]);
}

void test_buffer_too_small() {
  uint8_t buf[18];
  TEST_ASSERT_EQUAL(0, encodePosition(sample(), buf, sizeof(buf)));
}

void test_roundtrip() {
  uint8_t buf[kPacketSize];
  encodePosition(sample(), buf, sizeof(buf));
  Position d;
  TEST_ASSERT_TRUE(decodePosition(buf, sizeof(buf), d));
  TEST_ASSERT_EQUAL_UINT8(7, d.deviceId);
  TEST_ASSERT_EQUAL_UINT8(200, d.seq);
  TEST_ASSERT_EQUAL_UINT32(45319200UL, d.timeMs);
  TEST_ASSERT_EQUAL_INT32(471234567, d.latE7);
  TEST_ASSERT_EQUAL_INT32(-84567890, d.lonE7);
  TEST_ASSERT_EQUAL_UINT16(730, d.speedCms);        // 0.1 m/s Auflösung
  TEST_ASSERT_UINT16_WITHIN(71, 27050, d.courseCdeg);  // 1.4° Auflösung
  TEST_ASSERT_EQUAL_UINT8(17, d.sats);
  TEST_ASSERT_EQUAL(FIX_GNSS, d.fix);
  TEST_ASSERT_EQUAL_UINT16(8, d.hdopX10);
  TEST_ASSERT_UINT16_WITHIN(20, 3912, d.batteryMv);
}

void test_limits_are_clamped() {
  Position p = sample();
  p.speedCms = 4000;     // 40 m/s -> 25.5 m/s
  p.sats = 99;           // -> 63
  p.hdopX10 = 999;       // -> 25.5
  p.batteryMv = 9000;    // -> max. Stufe
  p.courseCdeg = 35990;  // knapp unter 360° -> 0
  uint8_t buf[kPacketSize];
  encodePosition(p, buf, sizeof(buf));
  Position d;
  decodePosition(buf, sizeof(buf), d);
  TEST_ASSERT_EQUAL_UINT16(2550, d.speedCms);
  TEST_ASSERT_EQUAL_UINT8(63, d.sats);
  TEST_ASSERT_EQUAL_UINT16(255, d.hdopX10);
  TEST_ASSERT_EQUAL_UINT16(3000 + 40 * 31, d.batteryMv);
  TEST_ASSERT_EQUAL_UINT16(0, d.courseCdeg);
  TEST_ASSERT_EQUAL_INT32(471234567, d.latE7);  // Nachbarfelder unverändert
}

void test_low_battery_and_unknown_time() {
  Position p = sample();
  p.batteryMv = 2900;
  p.timeMs = 0xFFFFFFFF;
  p.fix = FIX_NONE;
  uint8_t buf[kPacketSize];
  encodePosition(p, buf, sizeof(buf));
  Position d;
  decodePosition(buf, sizeof(buf), d);
  TEST_ASSERT_EQUAL_UINT16(0, d.batteryMv);
  TEST_ASSERT_TRUE(d.timeMs >= kMsPerDay);
  TEST_ASSERT_EQUAL(FIX_NONE, d.fix);
}

void test_decode_rejects_invalid() {
  uint8_t buf[kPacketSize];
  encodePosition(sample(), buf, sizeof(buf));
  Position d;
  TEST_ASSERT_FALSE(decodePosition(buf, kPacketSize - 1, d));
  buf[0] = 0x40;  // LoRaWAN Unconfirmed Data Up
  TEST_ASSERT_FALSE(decodePosition(buf, kPacketSize, d));
  buf[0] = 0xE0;
  buf[1] = (2 << 5) | 7;  // Version 2
  TEST_ASSERT_FALSE(decodePosition(buf, kPacketSize, d));
}

void test_fix_type_from_gga() {
  TEST_ASSERT_EQUAL(FIX_NONE, fixTypeFromGga(0, false));
  TEST_ASSERT_EQUAL(FIX_GNSS, fixTypeFromGga(1, true));
  TEST_ASSERT_EQUAL(FIX_DGNSS, fixTypeFromGga(2, true));
  TEST_ASSERT_EQUAL(FIX_ESTIMATED, fixTypeFromGga(6, true));
  TEST_ASSERT_EQUAL(FIX_GNSS, fixTypeFromGga(0, true));  // nur RMC vorhanden
}

void test_airtime() {
  // SF7 / 250 kHz / 19 Byte: 50.25 Symbole * 0.512 ms
  TEST_ASSERT_EQUAL_UINT32(25728, loraAirtimeUs(19, 7, 250000));
  // 16 bis 19 Byte brauchen gleich viele Symbole
  TEST_ASSERT_EQUAL_UINT32(loraAirtimeUs(19, 7, 250000), loraAirtimeUs(16, 7, 250000));
  TEST_ASSERT_TRUE(loraAirtimeUs(20, 7, 250000) > loraAirtimeUs(19, 7, 250000));
  // SF12 / 125 kHz / 13 Byte (leerer LoRaWAN-Frame), mit Low Data Rate Optimize
  TEST_ASSERT_EQUAL_UINT32(1155072, loraAirtimeUs(13, 12, 125000));
  TEST_ASSERT_EQUAL_UINT32(0, loraAirtimeUs(19, 13, 250000));
}

void test_duty_cycle_band() {
  TEST_ASSERT_EQUAL_UINT16(100, dutyCyclePermille(869525000, 250000));
  TEST_ASSERT_EQUAL_UINT16(100, dutyCyclePermille(869525000, 125000));
  TEST_ASSERT_EQUAL_UINT16(10, dutyCyclePermille(869525000, 500000));  // ragt aus dem Band
  TEST_ASSERT_EQUAL_UINT16(10, dutyCyclePermille(868100000, 125000));
}

void test_min_spacing() {
  TEST_ASSERT_EQUAL_UINT32(258, minTxSpacingMs(25728, 100));   // 10 %
  TEST_ASSERT_EQUAL_UINT32(2573, minTxSpacingMs(25728, 10));   // 1 %
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_size_and_header);
  RUN_TEST(test_buffer_too_small);
  RUN_TEST(test_roundtrip);
  RUN_TEST(test_limits_are_clamped);
  RUN_TEST(test_low_battery_and_unknown_time);
  RUN_TEST(test_decode_rejects_invalid);
  RUN_TEST(test_fix_type_from_gga);
  RUN_TEST(test_airtime);
  RUN_TEST(test_duty_cycle_band);
  RUN_TEST(test_min_spacing);
  return UNITY_END();
}
