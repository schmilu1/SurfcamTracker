/*
 * L76K_Probe – Test-Sketch für das GNSS-Modul des Dragino TrackerD
 *
 * Ermittelt am echten Gerät:
 *   1. Baudrate, auf der das Modul gerade sendet (9600 / 115200 / 38400 / 57600)
 *   2. Modul-Kennung ($PCAS06 -> L76K/CASIC, $PMTK605 -> L70/MediaTek)
 *   3. ausgegebene NMEA-Sätze und Talker-IDs
 *   4. ob 115200 Baud und reduzierte Ausgabe (nur RMC + GGA) übernommen werden
 *   5. tatsächlich erreichte Update-Rate bei 1, 2, 5 und 10 Hz
 *   6. danach laufend: Fix, Satelliten, HDOP, Position, Geschwindigkeit, Rate
 *
 * Board: ESP32 PICO-D4 (pico32), Flash Mode DIO, Arduino-ESP32 Core 3.x
 * Serieller Monitor: 115200 Baud, Zeilenende "Newline"
 *
 * Eingaben im seriellen Monitor:
 *   t          Test-Ablauf erneut starten
 *   e          Rohdaten-Ausgabe (NMEA) ein/aus
 *   PCAS10,0   beliebiger Befehl ohne '$' und Prüfsumme wird mit Prüfsumme gesendet
 *
 * Hinweis: Die Konfiguration wird NICHT im Modul gespeichert ($PCAS00 wird nicht
 * gesendet). Nach dem Abschalten der GNSS-Versorgung gelten wieder die Standardwerte,
 * sofern das Modul sie nicht über die Pufferbatterie (V_BCKP) behält.
 */
#include <Arduino.h>

// Pinbelegung laut Schaltplan TrackerD v1.2
static const int PIN_GPS_RX    = 9;   // ESP RX  <- Modul TXD
static const int PIN_GPS_TX    = 10;  // ESP TX  -> Modul RXD
static const int PIN_GPS_POWER = 12;  // HIGH = GNSS-Versorgung an (Q11/Q13)
static const int PIN_GPS_RESET = 25;  // HIGH = Reset aktiv (über BSS138 Q7)
static const int PIN_LORA_NSS  = 18;  // SX1276 abwählen
static const int PIN_LED_RED   = 13;
static const int PIN_LED_BLUE  = 2;
static const int PIN_LED_GREEN = 15;

HardwareSerial GPS(1);

// ---------------------------------------------------------------- Statistik

struct SentenceCount {
  char type[8];     // z. B. "GNRMC"
  uint32_t count;
};

static SentenceCount g_types[24];
static uint8_t  g_typeCount = 0;
static uint32_t g_lines = 0;
static uint32_t g_badChecksum = 0;
static uint32_t g_rmc = 0;
static uint32_t g_gga = 0;
static bool     g_echo = false;
static bool     g_echoTxt = false;
static bool     g_sawPmtk = false;

// Letzter Zustand aus RMC/GGA
static char   g_time[12] = "";
static char   g_status = 'V';
static int    g_fixQuality = 0;
static int    g_sats = 0;
static float  g_hdop = 0;
static double g_lat = 0, g_lon = 0;
static float  g_speedKmh = 0;
static float  g_course = 0;
static float  g_alt = 0;

static uint32_t g_currentBaud = 0;

static void resetStats() {
  g_typeCount = 0;
  g_lines = g_badChecksum = g_rmc = g_gga = 0;
}

static void countType(const char *type) {
  for (uint8_t i = 0; i < g_typeCount; i++) {
    if (strcmp(g_types[i].type, type) == 0) {
      g_types[i].count++;
      return;
    }
  }
  if (g_typeCount < sizeof(g_types) / sizeof(g_types[0])) {
    strncpy(g_types[g_typeCount].type, type, sizeof(g_types[0].type) - 1);
    g_types[g_typeCount].type[sizeof(g_types[0].type) - 1] = '\0';
    g_types[g_typeCount].count = 1;
    g_typeCount++;
  }
}

// ---------------------------------------------------------------- NMEA

static uint8_t nmeaChecksum(const char *body) {
  uint8_t c = 0;
  while (*body) c ^= (uint8_t)*body++;
  return c;
}

static void sendCmd(const char *body) {
  char buf[128];
  snprintf(buf, sizeof(buf), "$%s*%02X\r\n", body, nmeaChecksum(body));
  GPS.print(buf);
  GPS.flush();
  Serial.printf(">> %s", buf);
}

static int splitFields(char *s, char **fields, int maxFields) {
  int n = 0;
  fields[n++] = s;
  for (char *p = s; *p && n < maxFields; p++) {
    if (*p == ',') {
      *p = '\0';
      fields[n++] = p + 1;
    }
  }
  return n;
}

// ddmm.mmmm -> Grad
static double nmeaToDeg(const char *v, const char *hemi) {
  if (!v[0]) return 0;
  double raw = atof(v);
  int deg = (int)(raw / 100);
  double d = deg + (raw - deg * 100) / 60.0;
  if (hemi[0] == 'S' || hemi[0] == 'W') d = -d;
  return d;
}

static void handleSentence(char *line) {
  // line: "$GNRMC,....*hh" ohne Zeilenende
  char *star = strrchr(line, '*');
  if (line[0] != '$' || !star) return;
  *star = '\0';
  uint8_t want = (uint8_t)strtoul(star + 1, nullptr, 16);
  if (nmeaChecksum(line + 1) != want) {
    g_badChecksum++;
    return;
  }
  g_lines++;

  char *fields[24];
  int n = splitFields(line + 1, fields, 24);
  const char *type = fields[0];
  countType(type);

  if (strncmp(type, "PMTK", 4) == 0) g_sawPmtk = true;

  const char *t3 = strlen(type) >= 5 ? type + 2 : type;  // ohne Talker-ID
  if (strcmp(t3, "RMC") == 0 && n >= 10) {
    g_rmc++;
    strncpy(g_time, fields[1], sizeof(g_time) - 1);
    g_status = fields[2][0] ? fields[2][0] : 'V';
    g_lat = nmeaToDeg(fields[3], fields[4]);
    g_lon = nmeaToDeg(fields[5], fields[6]);
    g_speedKmh = atof(fields[7]) * 1.852f;
    g_course = atof(fields[8]);
  } else if (strcmp(t3, "GGA") == 0 && n >= 10) {
    g_gga++;
    g_fixQuality = atoi(fields[6]);
    g_sats = atoi(fields[7]);
    g_hdop = atof(fields[8]);
    g_alt = atof(fields[9]);
  } else if (strcmp(t3, "TXT") == 0 && g_echoTxt) {
    // $GPTXT,01,01,02,<Text>
    Serial.printf("   TXT: %s\r\n", n >= 5 ? fields[4] : "");
  }
}

static char     g_lineBuf[128];
static uint16_t g_lineLen = 0;

static void pollGps() {
  while (GPS.available()) {
    char c = (char)GPS.read();
    if (g_echo) Serial.write(c);
    if (c == '$') g_lineLen = 0;
    if (c == '\r' || c == '\n') {
      if (g_lineLen > 0) {
        g_lineBuf[g_lineLen] = '\0';
        handleSentence(g_lineBuf);
        g_lineLen = 0;
      }
    } else if (g_lineLen < sizeof(g_lineBuf) - 1) {
      g_lineBuf[g_lineLen++] = c;
    } else {
      g_lineLen = 0;  // Zeile zu lang -> verwerfen
    }
  }
}

static void pump(uint32_t ms) {
  uint32_t start = millis();
  while (millis() - start < ms) {
    pollGps();
    delay(1);
  }
}

// ---------------------------------------------------------------- Ablauf

static void openGps(uint32_t baud) {
  GPS.end();
  GPS.setRxBufferSize(2048);
  GPS.begin(baud, SERIAL_8N1, PIN_GPS_RX, PIN_GPS_TX);
  g_currentBaud = baud;
  g_lineLen = 0;
}

static void printTypes() {
  Serial.print("   Sätze: ");
  for (uint8_t i = 0; i < g_typeCount; i++) {
    Serial.printf("%s×%lu ", g_types[i].type, (unsigned long)g_types[i].count);
  }
  Serial.printf("\r\n   gültig: %lu, Prüfsummenfehler: %lu\r\n",
                (unsigned long)g_lines, (unsigned long)g_badChecksum);
}

static bool detectBaud() {
  const uint32_t bauds[] = {9600, 115200, 38400, 57600, 19200};
  for (uint32_t b : bauds) {
    openGps(b);
    resetStats();
    pump(2500);
    Serial.printf("   %6lu Baud: %lu gültige Sätze\r\n", (unsigned long)b, (unsigned long)g_lines);
    if (g_lines >= 3) return true;
  }
  return false;
}

static void identify() {
  Serial.println("\r\n[2] Modul-Kennung");
  g_echoTxt = true;
  g_sawPmtk = false;
  sendCmd("PCAS06,0");   // L76K: Firmware-Version
  pump(1500);
  sendCmd("PCAS06,1");   // L76K: Hardware-Modell
  pump(1500);
  sendCmd("PMTK605");    // L70 (MediaTek): Firmware-Version -> $PMTK705
  pump(1500);
  g_echoTxt = false;
  Serial.printf("   MediaTek-Antwort ($PMTK...): %s\r\n", g_sawPmtk ? "ja -> eher L70" : "nein");
}

static bool switchTo115200() {
  Serial.println("\r\n[4] Wechsel auf 115200 Baud");
  if (g_currentBaud == 115200) {
    Serial.println("   läuft bereits mit 115200");
    return true;
  }
  uint32_t old = g_currentBaud;
  sendCmd("PCAS01,5");
  delay(200);
  openGps(115200);
  resetStats();
  pump(2500);
  if (g_lines >= 2) {
    Serial.printf("   OK, %lu gültige Sätze bei 115200\r\n", (unsigned long)g_lines);
    return true;
  }
  Serial.println("   FEHLER: keine gültigen Sätze bei 115200, zurück auf alte Baudrate");
  openGps(old);
  return false;
}

static void testRates() {
  Serial.println("\r\n[5] Update-Rate (je 5 s gemessen, RMC-Sätze pro Sekunde)");
  const int intervals[] = {1000, 500, 200, 100};
  for (int ms : intervals) {
    char cmd[24];
    snprintf(cmd, sizeof(cmd), "PCAS02,%d", ms);
    sendCmd(cmd);
    pump(1500);  // Umschalten abwarten
    resetStats();
    uint32_t start = millis();
    pump(5000);
    float secs = (millis() - start) / 1000.0f;
    float rmcRate = g_rmc / secs;
    float ggaRate = g_gga / secs;
    float want = 1000.0f / ms;
    Serial.printf("   Soll %4.1f Hz -> RMC %4.1f Hz, GGA %4.1f Hz, Prüfsummenfehler %lu  %s\r\n",
                  want, rmcRate, ggaRate, (unsigned long)g_badChecksum,
                  rmcRate >= want * 0.9f ? "OK" : "nicht erreicht");
  }
}

static void runTests() {
  Serial.println("\r\n========== L76K_Probe ==========");

  Serial.println("\r\n[1] Baudrate suchen");
  if (!detectBaud()) {
    Serial.println("   FEHLER: keine NMEA-Daten. Versorgung (IO12), Reset (IO25) und Pins 9/10 prüfen.");
    return;
  }
  Serial.printf("   -> Modul sendet mit %lu Baud\r\n", (unsigned long)g_currentBaud);

  identify();

  Serial.println("\r\n[3] Standardausgabe (3 s)");
  resetStats();
  pump(3000);
  printTypes();

  bool fast = switchTo115200();

  Serial.println("\r\n    Ausgabe auf RMC + GGA reduzieren");
  // $PCAS03: Ausgabehäufigkeit je Satztyp (GGA,GLL,GSA,GSV,RMC,VTG,ZDA,ANT,DHV,LPS,...)
  sendCmd("PCAS03,1,0,0,0,1,0,0,0,0,0,,,0,0,,,,0");
  pump(1500);
  resetStats();
  pump(3000);
  printTypes();

  if (!fast) {
    Serial.println("   Hinweis: ohne 115200 Baud sind über ~5 Hz nicht möglich (9600 Baud ≈ 960 Byte/s)");
  }
  testRates();

  Serial.println("\r\n[6] Laufende Anzeige (1/s). Eingabe 't' wiederholt den Test, 'e' zeigt Rohdaten.");
  resetStats();
}

static void printStatus() {
  static uint32_t lastRmc = 0;
  uint32_t rate = g_rmc - lastRmc;
  lastRmc = g_rmc;
  Serial.printf("%s  %s fix=%d sats=%2d hdop=%4.1f  %.6f, %.6f  alt=%.0fm  %5.1f km/h %5.1f°  %lu Hz\r\n",
                g_time[0] ? g_time : "--------",
                g_status == 'A' ? "A" : "V",
                g_fixQuality, g_sats, g_hdop, g_lat, g_lon, g_alt,
                g_speedKmh, g_course, (unsigned long)rate);
  bool fix = (g_status == 'A');
  digitalWrite(PIN_LED_GREEN, fix ? HIGH : LOW);
  digitalWrite(PIN_LED_BLUE, fix ? LOW : HIGH);
}

static void handleUsbInput() {
  static char buf[96];
  static uint8_t len = 0;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r' || c == '\n') {
      if (len == 0) continue;
      buf[len] = '\0';
      len = 0;
      if (strcmp(buf, "t") == 0) {
        runTests();
      } else if (strcmp(buf, "e") == 0) {
        g_echo = !g_echo;
        Serial.printf("Rohdaten %s\r\n", g_echo ? "ein" : "aus");
      } else {
        const char *body = (buf[0] == '$') ? buf + 1 : buf;
        char *star = strchr((char *)body, '*');
        if (star) *star = '\0';
        g_echoTxt = true;
        sendCmd(body);
      }
    } else if (len < sizeof(buf) - 1) {
      buf[len++] = c;
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(PIN_LED_RED, OUTPUT);
  pinMode(PIN_LED_BLUE, OUTPUT);
  pinMode(PIN_LED_GREEN, OUTPUT);
  digitalWrite(PIN_LED_RED, LOW);
  digitalWrite(PIN_LED_GREEN, LOW);
  digitalWrite(PIN_LED_BLUE, HIGH);

  // LoRa-Chip abwählen, damit er den SPI-Bus nicht stört
  pinMode(PIN_LORA_NSS, OUTPUT);
  digitalWrite(PIN_LORA_NSS, HIGH);

  // GNSS einschalten: Reset freigeben, Versorgung an
  pinMode(PIN_GPS_RESET, OUTPUT);
  digitalWrite(PIN_GPS_RESET, LOW);
  pinMode(PIN_GPS_POWER, OUTPUT);
  digitalWrite(PIN_GPS_POWER, HIGH);
  delay(1000);  // Anlaufzeit des Moduls

  runTests();
}

void loop() {
  pollGps();
  handleUsbInput();

  static uint32_t last = 0;
  if (millis() - last >= 1000) {
    last = millis();
    printStatus();
  }
}
