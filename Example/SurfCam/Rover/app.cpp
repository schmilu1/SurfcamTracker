// Hauptlogik der Rover-Firmware (aufgerufen aus Rover.ino)
#include <Arduino.h>
#include <driver/rtc_io.h>
#include <esp_sleep.h>

#include "app.h"
#include "config.h"
#include "console.h"
#include "packet.h"

Gnss gnss(Serial1);
Radio radio;
TrackLog trackLog;

// ---------------------------------------------------------------- Zustand

static surfcam::GnssFix g_fix;
static bool g_haveFix = false;  // überhaupt schon eine Epoche empfangen
static bool g_newFix = false;   // seit dem letzten Senden neue Epoche
static uint8_t g_seq = 0;
static uint32_t g_lastTxMs = 0;
static bool g_txEver = false;

static float g_batteryEma = 0;
static uint32_t g_lastBatteryMs = 0;
static uint32_t g_batteryLowSince = 0;

static bool g_buttonDown = false;
static bool g_ignoreUntilRelease = false;
static uint32_t g_buttonSince = 0;

// ---------------------------------------------------------------- LEDs

struct Led {
  int pin;
  bool on;
  uint32_t offAt;
};

static Led g_red = {pins::LED_RED, false, 0};
static Led g_green = {pins::LED_GREEN, false, 0};
static Led g_blue = {pins::LED_BLUE, false, 0};
static uint32_t g_lastBlueMs = 0;

static void ledFlash(Led& l, uint32_t ms) {
  if (!settings.leds) return;
  digitalWrite(l.pin, HIGH);
  l.on = true;
  l.offAt = millis() + ms;
}

static void ledsOff() {
  for (Led* l : {&g_red, &g_green, &g_blue}) {
    digitalWrite(l->pin, LOW);
    l->on = false;
  }
}

static bool gnssAlive() {
  return gnss.lastEpochMs() != 0 && millis() - gnss.lastEpochMs() < 3000;
}

static bool fixValid() {
  return g_haveFix && g_fix.valid && millis() - gnss.lastEpochMs() < timing::FIX_TIMEOUT_MS;
}

static void ledsUpdate() {
  const uint32_t now = millis();
  for (Led* l : {&g_red, &g_green, &g_blue}) {
    if (l->on && (int32_t)(now - l->offAt) >= 0) {
      digitalWrite(l->pin, LOW);
      l->on = false;
    }
  }
  static uint32_t lastBeat = 0;
  if (now - lastBeat >= 1000) {
    lastBeat = now;
    if (!gnssAlive()) {
      ledFlash(g_red, 300);
    } else if (fixValid()) {
      ledFlash(g_green, 60);
    } else {
      ledFlash(g_red, 60);
    }
  }
}

// ---------------------------------------------------------------- Akku

static uint16_t readBatteryRaw() {
  // Spannungsteiler 100k (oben) / 470k (unten)
  return (uint16_t)((uint32_t)analogReadMilliVolts(pins::BATTERY) * 570 / 470);
}

uint16_t batteryMv() { return (uint16_t)g_batteryEma; }

static void updateBattery() {
  const uint32_t now = millis();
  if (now - g_lastBatteryMs < 1000) return;
  g_lastBatteryMs = now;
  const float v = readBatteryRaw();
  g_batteryEma = g_batteryEma == 0 ? v : g_batteryEma * 0.9f + v * 0.1f;
  if (g_batteryEma < timing::BATTERY_EMPTY_MV) {
    if (g_batteryLowSince == 0) {
      g_batteryLowSince = now;
    } else if (now - g_batteryLowSince >= timing::BATTERY_EMPTY_HOLD_MS) {
      powerOff("Akku leer");
    }
  } else {
    g_batteryLowSince = 0;
  }
}

static void showBatteryLevel() {
  const uint16_t mv = batteryMv();
  const int n = mv > 4000 ? 4 : mv > 3800 ? 3 : mv > 3600 ? 2 : 1;
  ledsOff();
  for (int i = 0; i < n; i++) {
    digitalWrite(pins::LED_GREEN, HIGH);
    delay(150);
    digitalWrite(pins::LED_GREEN, LOW);
    delay(150);
  }
}

// ---------------------------------------------------------------- Ein/Aus

static void enterDeepSleep() {
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0);  // Taste (active-low)
  rtc_gpio_pullup_en(GPIO_NUM_0);
  rtc_gpio_pulldown_dis(GPIO_NUM_0);
  Serial.flush();
  esp_deep_sleep_start();
}

void powerOff(const char* reason) {
  Serial.printf("[power] Ausschalten (%s)\r\n", reason);
  trackLog.close();
  gnss.end();  // GNSS-Versorgung aus (10k Pulldown hält IO12 im Sleep auf LOW)
  radio.sleep();
  ledsOff();
  digitalWrite(pins::LED_RED, HIGH);
  delay(1000);
  digitalWrite(pins::LED_RED, LOW);
  // Erst schlafen, wenn die Taste losgelassen ist, sonst wacht das Gerät sofort wieder auf
  while (digitalRead(pins::BUTTON) == LOW) delay(10);
  delay(50);
  enterDeepSleep();
}

static void handleButton() {
  const bool down = digitalRead(pins::BUTTON) == LOW;
  const uint32_t now = millis();
  if (g_ignoreUntilRelease) {
    if (!down) g_ignoreUntilRelease = false;
    return;
  }
  if (down && !g_buttonDown) {
    g_buttonDown = true;
    g_buttonSince = now;
  } else if (down && g_buttonDown) {
    if (now - g_buttonSince >= timing::LONG_PRESS_MS) powerOff("Taste");
  } else if (!down && g_buttonDown) {
    g_buttonDown = false;
    const uint32_t d = now - g_buttonSince;
    if (d >= 50 && d < 1000) {
      printStatus(Serial);
      showBatteryLevel();
    }
  }
}

// ---------------------------------------------------------------- Funk

bool applyRadioSettings() {
  return radio.begin(settings.freqHz, settings.sf, settings.bwHz, settings.txPowerDbm);
}

static void maybeSend() {
  const uint32_t now = millis();
  const bool valid = fixValid();
  const uint32_t interval = valid ? settings.txIntervalMs : timing::NO_FIX_TX_INTERVAL_MS;
  if (g_txEver && now - g_lastTxMs < interval) return;
  if (valid && !g_newFix) return;  // keine Position doppelt senden
  if (!radio.canSend()) return;    // Duty Cycle

  surfcam::Position p;
  p.deviceId = settings.deviceId;
  p.seq = g_seq;
  p.batteryMv = batteryMv();
  p.timeMs = g_haveFix ? g_fix.timeMs : surfcam::kTimeUnknown;
  p.sats = g_haveFix ? g_fix.sats : 0;
  p.hdopX10 = g_haveFix ? g_fix.hdopX10 : 0;
  if (valid) {
    p.latE7 = g_fix.latE7;
    p.lonE7 = g_fix.lonE7;
    p.speedCms = g_fix.speedCms;
    p.courseCdeg = g_fix.courseCdeg;
    p.fix = surfcam::fixTypeFromGga(g_fix.fixQuality, true);
  } else {
    p.fix = surfcam::FIX_NONE;
  }

  uint8_t buf[surfcam::kPacketSize];
  const size_t n = surfcam::encodePosition(p, buf, sizeof(buf));
  g_lastTxMs = now;
  g_txEver = true;
  if (radio.send(buf, n)) {
    g_seq++;
    g_newFix = false;
    if (now - g_lastBlueMs >= 1000) {
      g_lastBlueMs = now;
      ledFlash(g_blue, 30);
    }
  }
}

// ---------------------------------------------------------------- Status

void printStatus(Print& out) {
  out.printf("SurfCam Rover %s, Geräte-ID %u, Laufzeit %lu s\r\n", ROVER_FW_VERSION, settings.deviceId,
             (unsigned long)(millis() / 1000));
  const surfcam::NmeaStats& s = gnss.stats();
  out.printf("GNSS:  %lu Baud, Soll %u Hz, gemessen %.1f Hz, Sätze %lu, Prüfsummenfehler %lu, %s\r\n",
             (unsigned long)gnss.baud(), gnss.rateHz(), gnss.measuredHz(), (unsigned long)s.sentences,
             (unsigned long)s.checksumErrors, gnssAlive() ? "Daten OK" : "KEINE DATEN");
  if (g_haveFix) {
    const uint32_t t = g_fix.timeMs;
    if (t != surfcam::kTimeUnknown) {
      out.printf("       UTC %02lu:%02lu:%02lu.%03lu, ", (unsigned long)(t / 3600000),
                 (unsigned long)((t / 60000) % 60), (unsigned long)((t / 1000) % 60),
                 (unsigned long)(t % 1000));
    } else {
      out.print("       UTC unbekannt, ");
    }
    out.printf("Fix %s (Qualität %u), Satelliten %u, HDOP %u.%u\r\n", fixValid() ? "ja" : "nein",
               g_fix.fixQuality, g_fix.sats, g_fix.hdopX10 / 10, g_fix.hdopX10 % 10);
    if (g_fix.valid) {
      out.printf("       %.7f, %.7f, %.1f m, %.2f m/s, %.1f°\r\n", g_fix.latE7 / 1e7, g_fix.lonE7 / 1e7,
                 g_fix.altDm / 10.0, g_fix.speedCms / 100.0, g_fix.courseCdeg / 100.0);
    }
  }
  out.printf("Funk:  %s, %.3f MHz, SF%u, %lu kHz, %d dBm, gesendet %lu, Fehler %lu\r\n",
             radio.ok() ? "OK" : "FEHLER", settings.freqHz / 1e6, settings.sf,
             (unsigned long)(settings.bwHz / 1000), settings.txPowerDbm, (unsigned long)radio.txCount(),
             (unsigned long)radio.txFailed());
  out.printf("       Ziel-Abstand %u ms, min. %lu ms (Duty Cycle %u %%), Ø Duty Cycle %.2f %%\r\n",
             settings.txIntervalMs, (unsigned long)radio.minSpacingMs(surfcam::kPacketSize),
             radio.dutyPermille() / 10, radio.averageDutyPercent());
  out.printf("Akku:  %u mV\r\n", batteryMv());
  out.printf("Log:   %s, %lu Positionen, %u kB frei\r\n",
             trackLog.currentName()[0] ? trackLog.currentName() + 1 : "keine aktive Session",
             (unsigned long)trackLog.recordsInSession(), (unsigned)(trackLog.freeBytes() / 1024));
}

// ---------------------------------------------------------------- Ablauf

void appSetup() {
  rtc_gpio_deinit(GPIO_NUM_0);  // nach Wake-up aus ext0 wieder als normaler GPIO
  pinMode(pins::BUTTON, INPUT_PULLUP);
  Serial.begin(115200);

  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0) {
    // Nur einschalten, wenn die Taste 1 s gehalten wird (Schutz gegen versehentliches Drücken)
    const uint32_t t0 = millis();
    while (digitalRead(pins::BUTTON) == LOW && millis() - t0 < timing::WAKE_HOLD_MS) delay(10);
    if (millis() - t0 < timing::WAKE_HOLD_MS) enterDeepSleep();
    g_ignoreUntilRelease = true;
  }

  setCpuFrequencyMhz(80);

  for (int p : {pins::LED_RED, pins::LED_GREEN, pins::LED_BLUE, pins::BUZZER}) {
    pinMode(p, OUTPUT);
    digitalWrite(p, LOW);
  }
  digitalWrite(pins::LED_GREEN, HIGH);

  settings.load();
  delay(200);
  Serial.printf("\r\n=== SurfCam Rover %s ===\r\n", ROVER_FW_VERSION);

  g_batteryEma = readBatteryRaw();
  trackLog.begin();
  applyRadioSettings();
  gnss.begin(settings.gnssHz);

  digitalWrite(pins::LED_GREEN, LOW);
  printStatus(Serial);
  Serial.println("Bereit. AT+HELP zeigt die Befehle.");
}

void appLoop() {
  consolePoll();
  handleButton();

  surfcam::GnssFix f;
  while (gnss.poll(f)) {
    g_fix = f;
    g_haveFix = true;
    g_newFix = true;
    if (f.valid) trackLog.append(f, batteryMv(), settings.deviceId);
  }

  maybeSend();
  trackLog.tick();
  updateBattery();
  ledsUpdate();
  delay(1);
}
