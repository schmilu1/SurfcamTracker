#include "console.h"

#include <ctype.h>

#include "app.h"
#include "config.h"
#include "packet.h"

namespace {

char g_line[128];
size_t g_len = 0;

void ok() { Serial.println("OK"); }

void err(const char* msg) { Serial.printf("ERROR: %s\r\n", msg); }

bool parseU32(const char* s, uint32_t& v) {
  if (!s || !*s) return false;
  char* end = nullptr;
  unsigned long x = strtoul(s, &end, 10);
  if (*end != '\0') return false;
  v = x;
  return true;
}

void printHelp() {
  Serial.println(
      "AT               Verbindungstest\r\n"
      "ATI / AT+INFO    Status\r\n"
      "AT+ID=<0..31>    Geräte-ID im Funkpaket\r\n"
      "AT+RATE=<Hz>     GNSS-Rate: 1, 2, 4, 5, 10\r\n"
      "AT+TXINT=<ms>    Ziel-Sendeabstand bei Fix (Duty Cycle begrenzt zusätzlich)\r\n"
      "AT+TXP=<dBm>     Sendeleistung 2..20\r\n"
      "AT+FREQ=<Hz>     Frequenz 863000000..870000000\r\n"
      "AT+SF=<7..12>    Spreading Factor\r\n"
      "AT+BW=<Hz>       Bandbreite 125000, 250000, 500000\r\n"
      "AT+LED=<0|1>     Status-LEDs\r\n"
      "AT+NMEA=<0|1>    NMEA-Rohdaten anzeigen (nicht gespeichert)\r\n"
      "AT+GNSS=<cmd>    Befehl an L76K senden, z. B. AT+GNSS=PCAS06,0\r\n"
      "AT+BTN           Tastentest: 10 s lang Pegel von IO0 und IO25 anzeigen\r\n"
      "AT+LOG           Sessions im Flash auflisten\r\n"
      "AT+LOGDUMP[=<f>] Session als CSV ausgeben (ohne Name: neueste)\r\n"
      "AT+LOGDEL=<f>    Session löschen (ALL = alle)\r\n"
      "AT+SLEEP         Ausschalten (Aufwecken: Taste 1 s halten)\r\n"
      "ATZ              Neustart\r\n"
      "AT+FDR           Werkseinstellungen und Neustart\r\n"
      "Abfrage: AT+XXX? oder AT+XXX=?");
}

void printSpacingHint() {
  const uint32_t minMs = radio.minSpacingMs(surfcam::kPacketSize);
  Serial.printf("Duty Cycle %u %% -> min. Sendeabstand %lu ms (max. %.1f Pakete/s)\r\n",
                radio.dutyPermille() / 10, (unsigned long)minMs, minMs ? 1000.0f / minMs : 0.0f);
}

// Einstellungen mit Ganzzahlwert: Abfrage, Bereichsprüfung, Setzen
bool numericSetting(const char* name, const char* val, bool query, uint32_t current, uint32_t lo,
                    uint32_t hi, uint32_t& out) {
  if (query || !val) {
    Serial.printf("AT+%s=%lu\r\n", name, (unsigned long)current);
    return false;
  }
  uint32_t v;
  if (!parseU32(val, v) || v < lo || v > hi) {
    Serial.printf("ERROR: Wert %lu..%lu erwartet\r\n", (unsigned long)lo, (unsigned long)hi);
    return false;
  }
  out = v;
  return true;
}

void handle(char* cmd) {
  // Leerzeichen am Ende entfernen
  size_t l = strlen(cmd);
  while (l > 0 && isspace((unsigned char)cmd[l - 1])) cmd[--l] = '\0';
  while (*cmd == ' ') cmd++;
  if (!*cmd) return;

  char* val = nullptr;
  char* eq = strchr(cmd, '=');
  if (eq) {
    *eq = '\0';
    val = eq + 1;
  }
  for (char* p = cmd; *p; p++) *p = (char)toupper((unsigned char)*p);
  bool query = val && strcmp(val, "?") == 0;
  l = strlen(cmd);
  if (!val && l > 2 && cmd[l - 1] == '?') {
    cmd[l - 1] = '\0';
    query = true;
  }

  uint32_t v;
  if (strcmp(cmd, "AT") == 0) {
    ok();
  } else if (strcmp(cmd, "ATI") == 0 || strcmp(cmd, "AT+INFO") == 0) {
    printStatus(Serial);
    ok();
  } else if (strcmp(cmd, "AT+HELP") == 0) {
    printHelp();
  } else if (strcmp(cmd, "AT+ID") == 0) {
    if (numericSetting("ID", val, query, settings.deviceId, 0, 31, v)) {
      settings.deviceId = (uint8_t)v;
      settings.save();
      ok();
    }
  } else if (strcmp(cmd, "AT+RATE") == 0) {
    if (numericSetting("RATE", val, query, settings.gnssHz, 1, 10, v)) {
      if (v != 1 && v != 2 && v != 4 && v != 5 && v != 10) {
        err("1, 2, 4, 5 oder 10 Hz");
        return;
      }
      settings.gnssHz = gnss.setRate((uint8_t)v);
      settings.save();
      Serial.printf("GNSS-Rate %u Hz (gemessene Rate nach ca. 5 s mit ATI prüfen)\r\n", settings.gnssHz);
      ok();
    }
  } else if (strcmp(cmd, "AT+TXINT") == 0) {
    if (numericSetting("TXINT", val, query, settings.txIntervalMs, 50, 60000, v)) {
      settings.txIntervalMs = (uint16_t)v;
      settings.save();
      if (v < radio.minSpacingMs(surfcam::kPacketSize)) printSpacingHint();
      ok();
    }
  } else if (strcmp(cmd, "AT+TXP") == 0) {
    if (numericSetting("TXP", val, query, (uint32_t)settings.txPowerDbm, 2, 20, v)) {
      settings.txPowerDbm = (int8_t)v;
      settings.save();
      applyRadioSettings() ? ok() : err("Funk-Initialisierung fehlgeschlagen");
    }
  } else if (strcmp(cmd, "AT+FREQ") == 0) {
    if (numericSetting("FREQ", val, query, settings.freqHz, 863000000UL, 870000000UL, v)) {
      settings.freqHz = v;
      settings.save();
      if (applyRadioSettings()) {
        printSpacingHint();
        ok();
      } else {
        err("Funk-Initialisierung fehlgeschlagen");
      }
    }
  } else if (strcmp(cmd, "AT+SF") == 0) {
    if (numericSetting("SF", val, query, settings.sf, 7, 12, v)) {
      settings.sf = (uint8_t)v;
      settings.save();
      if (applyRadioSettings()) {
        printSpacingHint();
        ok();
      } else {
        err("Funk-Initialisierung fehlgeschlagen");
      }
    }
  } else if (strcmp(cmd, "AT+BW") == 0) {
    if (numericSetting("BW", val, query, settings.bwHz, 125000, 500000, v)) {
      if (v != 125000 && v != 250000 && v != 500000) {
        err("125000, 250000 oder 500000");
        return;
      }
      settings.bwHz = v;
      settings.save();
      if (applyRadioSettings()) {
        printSpacingHint();
        ok();
      } else {
        err("Funk-Initialisierung fehlgeschlagen");
      }
    }
  } else if (strcmp(cmd, "AT+LED") == 0) {
    if (numericSetting("LED", val, query, settings.leds ? 1 : 0, 0, 1, v)) {
      settings.leds = v == 1;
      settings.save();
      ok();
    }
  } else if (strcmp(cmd, "AT+NMEA") == 0) {
    if (numericSetting("NMEA", val, query, gnss.echo ? 1 : 0, 0, 1, v)) {
      gnss.echo = v == 1;
      ok();
    }
  } else if (strcmp(cmd, "AT+GNSS") == 0) {
    if (!val || !*val || query) {
      err("Befehl ohne $ und Prüfsumme angeben, z. B. AT+GNSS=PCAS06,0");
      return;
    }
    char* star = strchr(val, '*');
    if (star) *star = '\0';
    gnss.sendCommand(val[0] == '$' ? val + 1 : val);
    ok();
  } else if (strcmp(cmd, "AT+BTN") == 0) {
    // Diagnose: an welchem Pin hängt die Taste? (ältere Geräte IO0 LOW, neuere IO25 HIGH)
    Serial.println("Tastentest 10 s: Taste mehrmals drücken. Ausgabe bei jeder Änderung.");
    int last0 = -1, last25 = -1;
    const uint32_t t0 = millis();
    while (millis() - t0 < 10000) {
      const int io0 = digitalRead(pins::BUTTON), io25 = digitalRead(pins::BUTTON_HIGH);
      if (io0 != last0 || io25 != last25) {
        Serial.printf("%5lu ms  IO0=%d  IO25=%d  -> %s\r\n", (unsigned long)(millis() - t0), io0, io25,
                      buttonPressed() ? "gedrückt" : "losgelassen");
        last0 = io0;
        last25 = io25;
      }
      delay(10);
    }
    ok();
  } else if (strcmp(cmd, "AT+LOG") == 0) {
    trackLog.list(Serial);
    ok();
  } else if (strcmp(cmd, "AT+LOGDUMP") == 0) {
    // Ohne Namen: neueste Session
    char name[32];
    const char* file = val;
    if (!file || !*file || query) {
      if (!trackLog.latestName(name, sizeof(name))) {
        err("keine Session im Flash");
        return;
      }
      file = name;
    }
    if (trackLog.dump(file, Serial)) {
      ok();
    } else {
      Serial.printf("ERROR: Session \"%s\" nicht gefunden oder ungültig. Vorhanden:\r\n", file);
      trackLog.list(Serial);
    }
  } else if (strcmp(cmd, "AT+LOGDEL") == 0) {
    if (!val || !*val) {
      err("Dateiname oder ALL angeben");
    } else if (strcmp(val, "ALL") == 0 || strcmp(val, "all") == 0) {
      Serial.printf("%lu Dateien gelöscht\r\n", (unsigned long)trackLog.removeAll());
      ok();
    } else if (trackLog.remove(val)) {
      ok();
    } else {
      err("Datei nicht gefunden");
    }
  } else if (strcmp(cmd, "AT+SLEEP") == 0) {
    ok();
    powerOff("AT+SLEEP");
  } else if (strcmp(cmd, "ATZ") == 0) {
    ok();
    trackLog.close();
    Serial.flush();
    ESP.restart();
  } else if (strcmp(cmd, "AT+FDR") == 0) {
    settings.factoryReset();
    ok();
    trackLog.close();
    Serial.flush();
    ESP.restart();
  } else {
    err("unbekannter Befehl, siehe AT+HELP");
  }
}

}  // namespace

void consolePoll() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r' || c == '\n') {
      if (g_len > 0) {
        g_line[g_len] = '\0';
        g_len = 0;
        handle(g_line);
      }
    } else if (g_len < sizeof(g_line) - 1) {
      g_line[g_len++] = c;
    }
  }
}
