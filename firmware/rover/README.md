# SurfCam Rover – Firmware für den Dragino TrackerD

Der TrackerD wird am Surfer getragen. Er liest die Position vom GNSS-Modul (Quectel L76K), speichert jede gültige Position im internen Flash und sendet die jeweils neueste Position per **LoRa Punkt-zu-Punkt** an das Gateway am Strand. Der Rover rechnet nichts, er leitet nur weiter. Filter, Geometrie und Bildausschnitt laufen auf dem Raspberry Pi.

Diese Firmware ersetzt die Dragino-Firmware vollständig: kein LoRaWAN, kein BLE/WLAN, keine Alarm-, Schrittzähler- oder Temperaturfunktionen.

## Funk

| Parameter | Wert | Begründung |
|---|---|---|
| Frequenz | 869,525 MHz | Mitte des Bands 869,40–869,65 MHz: **10 % Duty Cycle, max. 500 mW ERP** (ETSI EN 300 220 / ERC 70-03) |
| Modulation | LoRa SF7, 250 kHz, CR 4/5, Präambel 8, CRC an, expliziter Header | 250 kHz ist die grösste Bandbreite, die ins Band passt |
| Sync-Word | 0x34 (öffentlich, wie LoRaWAN) | damit das SX1303-Gateway die Pakete empfängt |
| Paket | 19 Bytes, Sendezeit 25,7 ms | 16–19 Bytes brauchen gleich viele Symbole |
| Sendeabstand | ≥ 258 ms (10 % Duty Cycle), Standard 300 ms | **≈ 3,3 Positionen/s** |
| Sendeleistung | 17 dBm (einstellbar 2–20) | genug Reserve für 500 m über Wasser |

Die Firmware erzwingt den Duty Cycle selbst: Nach jeder Sendung wartet sie mindestens das Neunfache der Sendezeit. Liegt die eingestellte Frequenz ausserhalb des 10-%-Bands, gilt automatisch 1 %.

Ohne Fix sendet der Rover alle 2 s ein Lebenszeichen (Fix-Art 0, Akku, Satellitenzahl).

## Paketformat (für den Empfänger)

19 Bytes, Big Endian. Das erste Byte ist der LoRaWAN-MHDR für „Proprietary“ (`0xE0`). ChirpStack verwirft solche Frames, der Pi-Dienst liest sie roh aus dem MQTT-Uplink des Gateway Bridge.

| Byte | Inhalt |
|---|---|
| 0 | `0xE0` |
| 1 | Bits 7–5: Version (`1`), Bits 4–0: Geräte-ID (0–31) |
| 2 | Sequenznummer (0–255, läuft über) – Paketverluste erkennen |
| 3–6 | Bits 31–27: Akku-Stufe *n* (0 = unbekannt/≤ 3000 mV, sonst 3000 + 40·*n* mV); Bits 26–0: Zeit des Fixes in ms seit UTC-Mitternacht (`0x7FFFFFF` = unbekannt) |
| 7–10 | Breite, `int32`, Grad × 10⁷ |
| 11–14 | Länge, `int32`, Grad × 10⁷ |
| 15 | Geschwindigkeit, 0,1 m/s (max. 25,5) |
| 16 | Kurs, 360/256 Grad |
| 17 | Bits 7–6: Fix-Art (0 kein Fix, 1 GNSS, 2 DGNSS, 3 geschätzt), Bits 5–0: Satelliten (max. 63) |
| 18 | HDOP × 10 (max. 25,5) |

Referenz-Implementierung: [packet.h](packet.h) / [packet.cpp](packet.cpp) (`encodePosition` / `decodePosition`). Das Datum ist nicht enthalten, der Empfänger ergänzt es aus seiner eigenen UTC-Uhr.

## Bedienung

| Aktion | Wirkung |
|---|---|
| Akku anschliessen / Reset | startet sofort |
| Taste **3 s** halten | Ausschalten: rote LED 1 s, dann Deep Sleep (GNSS aus, LoRa im Sleep) |
| Taste **1 s** halten (im Sleep) | Einschalten (kürzeres Drücken weckt nur kurz und schläft wieder) |
| Taste kurz drücken | Status auf USB, Akkustand als 1–4 grüne Blinker |

Die Taste hängt je nach Board-Revision an **IO0** (ältere Geräte, gedrückt = LOW) oder an **IO25** (neuere Geräte, gedrückt = HIGH). Die Firmware liest beide und wacht über beide auf (ext0 bzw. ext1). Auf Board v1.2 ist IO25 der GNSS-Reset; er wird als Eingang mit Pulldown betrieben und bleibt so inaktiv. `AT+BTN` zeigt 10 s lang die Pegel beider Pins.

| LED (1× pro Sekunde) | Bedeutung |
|---|---|
| grün kurz | GNSS-Fix |
| rot kurz | kein Fix |
| rot lang | keine Daten vom GNSS-Modul |
| blau kurz | Funkpaket gesendet (höchstens 1× pro Sekunde angezeigt) |

Unter 3,3 V Akkuspannung (10 s lang) schaltet sich der Rover selbst ab.

## AT-Befehle (USB, 115200 Baud, Zeilenende `\n`)

| Befehl | Beschreibung | Standard |
|---|---|---|
| `AT` | Verbindungstest | |
| `ATI` / `AT+INFO` | Status: GNSS-Rate (Soll/gemessen), Fix, Funk, Duty Cycle, Akku, Log | |
| `AT+ID=<0..31>` | Geräte-ID im Paket | 1 |
| `AT+RATE=<1\|2\|4\|5\|10>` | GNSS-Rate in Hz (wirkt sofort) | 5 |
| `AT+TXINT=<ms>` | Ziel-Sendeabstand bei Fix | 300 |
| `AT+TXP=<2..20>` | Sendeleistung in dBm | 17 |
| `AT+FREQ=<Hz>` | Frequenz (863–870 MHz) | 869525000 |
| `AT+SF=<7..12>` | Spreading Factor | 7 |
| `AT+BW=<125000\|250000\|500000>` | Bandbreite | 250000 |
| `AT+LED=<0\|1>` | Status-LEDs | 1 |
| `AT+NMEA=<0\|1>` | NMEA-Rohdaten anzeigen (nicht gespeichert) | 0 |
| `AT+BTN` | Tastentest: 10 s lang Pegel von IO0 und IO25 anzeigen | |
| `AT+GNSS=<befehl>` | Befehl ans L76K, ohne `$` und Prüfsumme, z. B. `AT+GNSS=PCAS06,0` | |
| `AT+LOG` | Sessions im Flash auflisten | |
| `AT+LOGDUMP` / `AT+LOGDUMP=<datei>` | neueste bzw. angegebene Session als CSV ausgeben (Name aus `AT+LOG`, z. B. `261105_214349.trk`) | |
| `AT+LOGDEL=<datei>` / `AT+LOGDEL=ALL` | Session(s) löschen | |
| `AT+SLEEP` | Ausschalten | |
| `ATZ` | Neustart | |
| `AT+FDR` | Werkseinstellungen, Neustart | |

Werte abfragen mit `AT+XXX?` oder `AT+XXX=?`. Einstellungen werden sofort im NVS gespeichert.

## Positionslog

Jede gültige Position landet im Flash (LittleFS, ca. 1,4 MB), eine Datei pro Session ab dem ersten Fix: `YYMMDD_hhmmss.trk` (UTC). Geschrieben wird alle 5 s bzw. nach 64 Positionen, bei Stromausfall gehen also höchstens 5 s verloren.

| GNSS-Rate | Datenrate | Kapazität |
|---|---|---|
| 5 Hz | 100 B/s | ≈ 3,8 h |
| 10 Hz | 200 B/s | ≈ 1,9 h |

Wird der Platz knapp, wird die älteste Session gelöscht. Die laufende Session wird nie gelöscht; ist nur noch sie übrig, stoppt die Aufzeichnung.

**Auslesen:** `AT+LOG` zeigt die Sessions, `AT+LOGDUMP` gibt die neueste als CSV im Monitor aus (`AT+LOGDUMP=<datei>` eine bestimmte). Der Parameter ist der Name der Datei im Flash, nicht eine Zieldatei.

In eine Datei auf dem Mac speichern: Monitor mit Mitschnitt starten, `AT+LOGDUMP` eingeben, Monitor beenden. Die Datei `platformio-device-monitor-*.log` liegt im aktuellen Ordner; die CSV-Zeilen beginnen nach der Zeile `date,utc,...`.

```
pio device monitor -f send_on_enter -f log2file
```

CSV-Spalten:

```
date,utc,time_ms,lat,lon,speed_mps,course_deg,sats,fix,hdop,battery_mv
2026-11-05,21:21:19.900,76879900,47.5529780,8.8972410,0.00,256.70,6,1,4.3,3950
```

Binärformat ([tracklog.h](tracklog.h), Little Endian): 12-Byte-Kopf (`"SCTL"`, Version, Geräte-ID, Datensatzgrösse, Datum DDMMYY), danach Datensätze à 20 Bytes. `time_ms` läuft über Mitternacht hinaus weiter.

## Build

### PlatformIO (VS-Code-Erweiterung oder CLI)

Lädt Plattform, Core 3.3.12 und RadioLib 7.8.1 automatisch.

```
pio run -e trackerd -t upload
pio device monitor
pio test -e native        # 27 Unit-Tests, ohne Hardware
```

### Arduino IDE (alternativ)

1. Boardverwaltung: **esp32 by Espressif Systems 3.3.12** (getestet wird gegen diese Version; nicht die Vorabversion 4.0.0-RC1)
2. Board **ESP32 PICO-D4**, Flash Mode **DIO**, Partition Scheme **Default 4MB with spiffs**
3. Bibliothek **RadioLib 7.8.1** von Jan Gromes installieren
4. `firmware/rover/rover.ino` öffnen und hochladen

## Ablauf

```
Start ─► GNSS an, Baudrate erkennen (115200/9600), auf 115200 Baud, RMC+GGA, AT+RATE Hz
      ─► LoRa initialisieren, LittleFS mounten
Loop ─► NMEA lesen ─► Epoche (RMC+GGA mit gleicher Zeit)
                    ├─► gültig? ─► ins Log
                    └─► neueste Epoche merken
     ─► Sendeabstand und Duty Cycle erlaubt? ─► 19-Byte-Paket senden, LoRa schlafen legen
     ─► Taste, Akku, LEDs
```

## Messwerte und Annahmen

**Am Gerät gemessen (Test-Sketch `firmware/l76k_probe`, 05.11.2026, drinnen/Fenster):**
- 115200 Baud und 10 Hz werden übernommen und erreicht.
- 6 Satelliten, HDOP 4,3: Die Position wanderte in 15 s um etwa 14 m, während die gemeldete Geschwindigkeit bei 0,0 km/h und der Kurs fest blieben. Das L76K hält im Stillstand Geschwindigkeit und Kurs fest (Static Hold). Ob das langsames Paddeln betrifft, muss draussen geprüft werden. Der Pi sollte die Geschwindigkeit zusätzlich aus den Positionen ableiten.

**Am Gerät gemessen (Rover-Firmware 0.1.0, 05.11.2026, Fenster):**
- 5,0 Hz gemessen, nur `$GNGGA`/`$GNRMC`, Log mit 5 Positionen pro Sekunde.
- Mit 11–12 Satelliten und HDOP 1,0 blieb die Position über 1,5 min innerhalb von etwa 1 m. Mit 6 Satelliten driftete sie in 15 s um etwa 8 m, bei gemeldeter Geschwindigkeit 0,00.
- Bei schlechtem Empfang meldete das L76K einen Sprung von etwa 215 m innerhalb von 200 ms als gültigen Fix (Höhe fiel dabei auf −47,1 m = Ellipsoidhöhe 0). Ausreisser muss der Empfänger verwerfen.
- Vereinzelt fehlt in einer Epoche der GGA-Satz (2 von 972 Epochen in 194 s). Die Prüfsummenfehler und zu langen Zeilen blieben dabei bei 0, Zeichen gehen also nicht am UART verloren; das L76K lässt den GGA selbst weg, vor allem während es nach dem Start seinen Takt justiert (Zeitstempel wie `.133`, `.483`). Der Parser übernimmt dann Qualität, Satelliten, HDOP und Höhe vom letzten GGA (höchstens 2 s alt). `ATI` zählt solche Epochen („ohne GGA“).
- Direkt nach dem Start meldet das L76K etwa 1,5 s lang Qualität 6 („geschätzt“) mit eingefrorener Position (Warmstart aus V_BCKP). Im Funkpaket als Fix-Art 3 markiert.
- Geschwindigkeit: 0,8–1,7 m/s beim Gehen werden mit passendem Kurs gemeldet. Langsames Wandern um etwa 0,25 m/s meldet das L76K als 0,00 (Static Hold), der Kurs bleibt dann stehen.
- Mit nur 4 Satelliten entsteht eine Scheinbewegung von etwa 10 m bei gemeldetem HDOP 1,3, danach 14,1. Fixes mit wenigen Satelliten bzw. hohem HDOP muss der Empfänger verwerfen oder abwerten.

**Taste (06.11.2026):** Mit Version 0.1.0 liess sich das Gerät per Taste weder aus- noch einschalten (`AT+SLEEP` funktionierte). Die Taste hängt bei diesem Gerät nicht an IO0. Nach der alten Dragino-Firmware nutzen neuere Geräte IO25 (active-high), was 0.1.0 als GNSS-Reset-Ausgang auf LOW gehalten hatte.

**Nicht verifiziert:**
- Feldreihenfolge von `$PCAS03` (RMC+GGA). Ob nur noch RMC und GGA kommen, zeigt `AT+NMEA=1`.
- Empfang durch das SX1303-Gateway auf 869,525 MHz / 250 kHz und Weitergabe der Rohframes über den ChirpStack Gateway Bridge – wird mit dem Pi-Teil geprüft.
- Aufwecken über IO0 (Boot-Pin) aus dem Deep Sleep. Die Dragino-Firmware nutzt denselben Pin zum Aufwecken.
- Stromverbrauch: grob geschätzt 50–70 mA im Betrieb (ESP32 bei 80 MHz, GNSS, LoRa mit 10 % Sendezeit). Laufzeit daher vor dem ersten Einsatz messen.
- Akkuspannung: Teiler 100k/470k laut Schaltplan, nicht kalibriert.

Die Firmware ist **noch nicht auf dem Gerät getestet**. Geprüft sind: Build mit `platformio/espressif32@8.0.0` (Arduino-ESP32 3.3.12) und RadioLib 7.8.1 ohne Warnungen (RAM 8,4 %, Flash 35,6 %) sowie die Unit-Tests (27/27). Beim Start vergleicht die Firmware ihre Sendezeit-Berechnung mit `getTimeOnAir()` von RadioLib und warnt bei mehr als 5 % Abweichung.
