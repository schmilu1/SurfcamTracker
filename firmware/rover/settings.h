// Einstellungen, die per AT-Befehl geändert und im NVS gespeichert werden
#pragma once

#include <stdint.h>

#include "config.h"

struct Settings {
  uint8_t deviceId = defaults::DEVICE_ID;
  uint8_t gnssHz = defaults::GNSS_HZ;
  uint16_t txIntervalMs = defaults::TX_INTERVAL_MS;
  int8_t txPowerDbm = defaults::TX_POWER_DBM;
  uint32_t freqHz = defaults::FREQ_HZ;
  uint8_t sf = defaults::SF;
  uint32_t bwHz = defaults::BW_HZ;
  bool leds = defaults::LEDS;
  uint8_t logHz = defaults::LOG_HZ;
  bool logMotion = defaults::LOG_MOTION;

  void load();
  void save() const;
  void factoryReset();
};

extern Settings settings;
