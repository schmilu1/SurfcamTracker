#include "settings.h"

#include <Preferences.h>

Settings settings;

static const char* kNamespace = "surfcam";

void Settings::load() {
  Preferences p;
  if (!p.begin(kNamespace, true)) return;  // noch nichts gespeichert -> Standardwerte
  deviceId = p.getUChar("id", deviceId);
  gnssHz = p.getUChar("gnssHz", gnssHz);
  txIntervalMs = p.getUShort("txIntv", txIntervalMs);  // neuer Schlüssel: 0 = automatisch
  txPowerDbm = p.getChar("txPow", txPowerDbm);
  freqHz = p.getULong("freq", freqHz);
  sf = p.getUChar("sf", sf);
  bwHz = p.getULong("bw", bwHz);
  leds = p.getBool("leds", leds);
  logHz = p.getUChar("logHz", logHz);
  p.end();
}

void Settings::save() const {
  Preferences p;
  if (!p.begin(kNamespace, false)) return;
  p.putUChar("id", deviceId);
  p.putUChar("gnssHz", gnssHz);
  p.putUShort("txIntv", txIntervalMs);
  p.putChar("txPow", txPowerDbm);
  p.putULong("freq", freqHz);
  p.putUChar("sf", sf);
  p.putULong("bw", bwHz);
  p.putBool("leds", leds);
  p.putUChar("logHz", logHz);
  p.end();
}

void Settings::factoryReset() {
  Preferences p;
  if (p.begin(kNamespace, false)) {
    p.clear();
    p.end();
  }
  *this = Settings();
}
