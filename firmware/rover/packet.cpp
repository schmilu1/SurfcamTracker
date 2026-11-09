#include "packet.h"

#include <math.h>

namespace surfcam {

namespace {

void putU32(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24);
  p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);
  p[3] = (uint8_t)v;
}

uint32_t getU32(const uint8_t* p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

uint32_t clampU32(uint32_t v, uint32_t maxV) { return v > maxV ? maxV : v; }

}  // namespace

size_t encodePosition(const Position& p, uint8_t* out, size_t outLen) {
  if (outLen < kPacketSize) return 0;

  uint32_t battStep = 0;
  if (p.batteryMv > 3000) battStep = clampU32((p.batteryMv - 3000 + 20) / 40, 31);
  uint32_t timeMs = p.timeMs < kMsPerDay ? p.timeMs : 0x7FFFFFF;  // 0x7FFFFFF = unbekannt

  uint32_t speed = clampU32((p.speedCms + 5) / 10, 255);
  uint32_t course = ((uint32_t)(p.courseCdeg % 36000) * 256 + 18000) / 36000;
  uint32_t hdop = clampU32(p.hdopX10, 255);
  uint32_t sats = clampU32(p.sats, 63);

  out[0] = kMhdrProprietary;
  out[1] = (uint8_t)((kPacketVersion << 5) | (p.deviceId & 0x1F));
  out[2] = p.seq;
  putU32(out + 3, (battStep << 27) | (timeMs & 0x7FFFFFF));
  putU32(out + 7, (uint32_t)p.latE7);
  putU32(out + 11, (uint32_t)p.lonE7);
  out[15] = (uint8_t)speed;
  out[16] = (uint8_t)(course & 0xFF);
  out[17] = (uint8_t)(((uint8_t)p.fix & 0x03) << 6 | sats);
  out[18] = (uint8_t)hdop;
  if (p.motion) {
    out[19] = (uint8_t)clampU32((p.motionRmsMg + 25) / 50, 254);
    out[20] = (uint8_t)clampU32((p.motionPeakMg + 50) / 100, 254);
  } else {
    out[19] = out[20] = kMotionUnknown;
  }
  const uint32_t tilt = p.motion ? (clampU32(p.tiltDeg, 180) + 1) / 3 : kTiltUnknown;  // 3 Grad, gerundet
  out[21] = (uint8_t)(((p.buttonCount & 0x03) << 6) | tilt);
  return kPacketSize;
}

bool decodePosition(const uint8_t* in, size_t len, Position& p) {
  if (len != kPacketSize) return false;
  if (in[0] != kMhdrProprietary) return false;
  if ((in[1] >> 5) != kPacketVersion) return false;

  p.deviceId = in[1] & 0x1F;
  p.seq = in[2];
  uint32_t w = getU32(in + 3);
  uint32_t battStep = w >> 27;
  p.batteryMv = battStep ? (uint16_t)(3000 + 40 * battStep) : 0;
  p.timeMs = w & 0x7FFFFFF;
  p.latE7 = (int32_t)getU32(in + 7);
  p.lonE7 = (int32_t)getU32(in + 11);
  p.speedCms = (uint16_t)(in[15] * 10);
  p.courseCdeg = (uint16_t)(((uint32_t)in[16] * 36000 + 128) / 256);
  p.fix = (FixType)(in[17] >> 6);
  p.sats = in[17] & 0x3F;
  p.hdopX10 = in[18];
  const uint8_t tilt = in[21] & 0x3F;
  p.buttonCount = in[21] >> 6;
  p.motion = in[19] != kMotionUnknown && in[20] != kMotionUnknown && tilt != kTiltUnknown;
  p.motionRmsMg = p.motion ? (uint16_t)(in[19] * 50) : 0;
  p.motionPeakMg = p.motion ? (uint16_t)(in[20] * 100) : 0;
  p.tiltDeg = p.motion ? (uint8_t)(tilt * 3) : 0;
  return true;
}

FixType fixTypeFromGga(uint8_t q, bool rmcValid) {
  switch (q) {
    case 1: return FIX_GNSS;
    case 2:
    case 4:
    case 5: return FIX_DGNSS;   // DGPS, RTK fixed, RTK float
    case 6: return FIX_ESTIMATED;
    case 0: return rmcValid ? FIX_GNSS : FIX_NONE;
    default: return rmcValid ? FIX_GNSS : FIX_NONE;
  }
}

uint32_t loraAirtimeUs(size_t payloadLen, uint8_t sf, uint32_t bwHz, uint8_t crDenom,
                       uint16_t preambleLen, bool crc, bool explicitHeader) {
  if (sf < 6 || sf > 12 || bwHz == 0 || crDenom < 5 || crDenom > 8) return 0;
  const double tSym = (double)(1UL << sf) / (double)bwHz;  // Sekunden
  const int de = (bwHz <= 125000 && sf >= 11) ? 1 : 0;   // Low Data Rate Optimize
  const int ih = explicitHeader ? 0 : 1;
  const int cr = crDenom - 4;
  const double num = 8.0 * payloadLen - 4.0 * sf + 28 + 16 * (crc ? 1 : 0) - 20 * ih;
  const double den = 4.0 * (sf - 2 * de);
  double extra = ceil(num / den) * (cr + 4);
  if (extra < 0) extra = 0;
  const double nPayload = 8 + extra;
  const double tPreamble = (preambleLen + 4.25) * tSym;
  return (uint32_t)lround((tPreamble + nPayload * tSym) * 1e6);
}

uint16_t dutyCyclePermille(uint32_t freqHz, uint32_t bwHz, int8_t txPowerDbm) {
  const uint32_t lo = freqHz - bwHz / 2;
  const uint32_t hi = freqHz + bwHz / 2;
  if (lo >= 869400000UL && hi <= 869650000UL) return 100;
  if (lo >= 869700000UL && hi <= 870000000UL && txPowerDbm <= kMaxDbmNoDutyBand) return 1000;
  return 10;
}

uint32_t minTxSpacingMs(uint32_t airtimeUs, uint16_t dutyPermille) {
  if (dutyPermille == 0) return UINT32_MAX;
  // Sendezeit / Duty Cycle, aufgerundet
  uint64_t us = (uint64_t)airtimeUs * 1000ULL / dutyPermille;
  return (uint32_t)((us + 999) / 1000);
}

uint32_t txPeriodEpochs(uint32_t minIntervalMs, uint32_t epochMs, uint8_t slots) {
  if (epochMs == 0) return 1;
  uint32_t n = (minIntervalMs + epochMs - 1) / epochMs;
  if (n < slots) n = slots;
  return n < 1 ? 1 : n;
}

bool isTxEpoch(uint32_t timeMs, uint32_t epochMs, uint32_t periodEpochs, uint8_t slot) {
  if (timeMs >= kMsPerDay || epochMs == 0 || periodEpochs == 0) return false;
  // runden: das Modul meldet Zeiten wie 12:00:00.100, aber zur Sicherheit auch 12:00:00.099
  const uint32_t idx = (timeMs + epochMs / 2) / epochMs;
  return idx % periodEpochs == slot % periodEpochs;
}

}  // namespace surfcam
