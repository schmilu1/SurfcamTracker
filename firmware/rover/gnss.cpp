#include "gnss.h"

#include "config.h"

using surfcam::NmeaParser;

void Gnss::open(uint32_t baud) {
  ser_.end();
  ser_.setRxBufferSize(2048);  // muss vor begin() stehen
  ser_.begin(baud, SERIAL_8N1, pins::GPS_RX, pins::GPS_TX);
  baud_ = baud;
  parser_.reset();
}

bool Gnss::waitForSentences(uint32_t timeoutMs, uint32_t count) {
  const uint32_t base = parser_.stats().sentences;
  const uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    while (ser_.available()) parser_.feed((char)ser_.read());
    if (parser_.stats().sentences - base >= count) return true;
    delay(2);
  }
  return false;
}

void Gnss::sendCommand(const char* body) {
  char buf[96];
  snprintf(buf, sizeof(buf), "$%s*%02X\r\n", body, NmeaParser::checksum(body));
  ser_.print(buf);
  ser_.flush();
  if (echo) Serial.printf(">> %s", buf);
}

bool Gnss::begin(uint8_t rateHz) {
  pinMode(pins::GPS_RESET, OUTPUT);
  digitalWrite(pins::GPS_RESET, LOW);  // Reset freigeben
  pinMode(pins::GPS_POWER, OUTPUT);
  digitalWrite(pins::GPS_POWER, HIGH);
  delay(300);

  // Nach einem Neustart des ESP32 kann das Modul (über V_BCKP) noch auf 115200 stehen
  bool found = false;
  const uint32_t bauds[] = {115200, 9600};
  for (uint32_t b : bauds) {
    open(b);
    if (waitForSentences(1500, 2)) {
      found = true;
      break;
    }
  }
  if (!found) {
    open(9600);
    Serial.println("[gnss] keine NMEA-Daten empfangen");
  }

  // Nur RMC und GGA ausgeben (GGA,GLL,GSA,GSV,RMC,VTG,ZDA,ANT,DHV,LPS,..,..,UTC,GST,..,..,..,TIM)
  sendCommand("PCAS03,1,0,0,0,1,0,0,0,0,0,,,0,0,,,,0");

  if (baud_ != 115200) {
    const uint32_t old = baud_;
    sendCommand("PCAS01,5");  // 115200 Baud
    delay(100);
    open(115200);
    if (!waitForSentences(1500, 2)) {
      Serial.printf("[gnss] 115200 Baud nicht übernommen, bleibe bei %lu\r\n", (unsigned long)old);
      open(old);
    }
  }

  setRate(rateHz);
  rateWindowStart_ = millis();
  rateWindowEpochs_ = parser_.stats().epochs;
  Serial.printf("[gnss] %s, %lu Baud, %u Hz eingestellt\r\n", found ? "OK" : "FEHLER",
                (unsigned long)baud_, rateHz_);
  return found;
}

void Gnss::end() {
  ser_.end();
  digitalWrite(pins::GPS_POWER, LOW);
}

uint8_t Gnss::setRate(uint8_t hz) {
  // L76K akzeptiert 1000, 500, 250, 200 und 100 ms
  uint16_t intervalMs;
  if (hz >= 10) {
    rateHz_ = 10;
    intervalMs = 100;
  } else if (hz >= 5) {
    rateHz_ = 5;
    intervalMs = 200;
  } else if (hz >= 4) {
    rateHz_ = 4;
    intervalMs = 250;
  } else if (hz >= 2) {
    rateHz_ = 2;
    intervalMs = 500;
  } else {
    rateHz_ = 1;
    intervalMs = 1000;
  }
  if (baud_ < 115200 && rateHz_ > 5) {
    Serial.println("[gnss] bei 9600 Baud höchstens 5 Hz");
    rateHz_ = 5;
    intervalMs = 200;
  }
  char cmd[16];
  snprintf(cmd, sizeof(cmd), "PCAS02,%u", intervalMs);
  sendCommand(cmd);
  return rateHz_;
}

bool Gnss::poll(surfcam::GnssFix& out) {
  bool got = false;
  while (ser_.available()) {
    char c = (char)ser_.read();
    if (echo) Serial.write(c);
    if (parser_.feed(c)) {
      out = parser_.epoch();
      lastEpochMs_ = millis();
      got = true;
      break;
    }
  }

  const uint32_t now = millis();
  if (now - rateWindowStart_ >= 5000) {
    const uint32_t e = parser_.stats().epochs;
    measuredHz_ = (e - rateWindowEpochs_) * 1000.0f / (now - rateWindowStart_);
    rateWindowStart_ = now;
    rateWindowEpochs_ = e;
  }
  return got;
}
