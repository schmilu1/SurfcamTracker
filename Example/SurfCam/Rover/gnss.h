// Steuerung des GNSS-Moduls Quectel L76K (CASIC-Befehle $PCASxx)
#pragma once

#include <Arduino.h>

#include "nmea.h"

class Gnss {
 public:
  explicit Gnss(HardwareSerial& serial) : ser_(serial) {}

  // Versorgung einschalten, Baudrate erkennen, auf 115200 Baud / RMC+GGA / rateHz einstellen.
  // false, wenn das Modul keine NMEA-Daten liefert.
  bool begin(uint8_t rateHz);
  void end();

  // Liest verfügbare Zeichen. true, wenn eine Epoche fertig ist (dann in 'out').
  // Pro Aufruf höchstens eine Epoche -> in einer Schleife aufrufen.
  bool poll(surfcam::GnssFix& out);

  // 1, 2, 4, 5 oder 10 Hz (andere Werte werden abgerundet). Rückgabe: eingestellte Rate.
  uint8_t setRate(uint8_t hz);
  void sendCommand(const char* body);

  uint32_t baud() const { return baud_; }
  uint8_t rateHz() const { return rateHz_; }
  float measuredHz() const { return measuredHz_; }
  uint32_t lastEpochMs() const { return lastEpochMs_; }
  const surfcam::NmeaStats& stats() const { return parser_.stats(); }

  bool echo = false;  // NMEA-Rohdaten auf USB ausgeben

 private:
  void open(uint32_t baud);
  bool waitForSentences(uint32_t timeoutMs, uint32_t count);

  HardwareSerial& ser_;
  surfcam::NmeaParser parser_;
  uint32_t baud_ = 0;
  uint8_t rateHz_ = 1;
  float measuredHz_ = 0;
  uint32_t lastEpochMs_ = 0;
  uint32_t rateWindowStart_ = 0;
  uint32_t rateWindowEpochs_ = 0;
};
