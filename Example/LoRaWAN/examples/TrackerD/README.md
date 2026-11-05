# TrackerD-Firmware (Fork `SurfCam`)

LoRaWAN-Firmware für den Dragino TrackerD (ESP32-PICO-D4, SX1276, GNSS) auf Basis des Dragino-Beispiels `TrackerD` (Firmware-Kennung `TrackerD v1.4.9`, TrackerD-LS `v1.0.3`).

Der Fork portiert die Firmware auf **Arduino-ESP32 Core 3.x** und **MCCI LMIC 5.x**, behebt mehrere Fehler und ersetzt die BLE-Ortung durch WLAN-Ortung.

- AT-Befehle und Downlinks: **[AT_COMMANDS.md](AT_COMMANDS.md)**
- Schaltplan: [`hardware/TrackerD v1.2.sch.pdf`](../../../../hardware/)

## Unterschiede zum Dragino-Original

| Bereich | Änderung |
|---|---|
| Plattform | Arduino-ESP32 Core 3.x (neue Timer- und LEDC-API), MCCI LMIC 5.x (`lmic_hal_*`) |
| BLE | **deaktiviert**. Modus 2/3 und `AT+BG` nutzen WLAN; BLE-Modi werden per AT/Downlink abgelehnt und alte Konfigurationen beim Start auf WLAN umgestellt |
| WLAN-Scan | neu geschrieben: keine Pufferüberläufe mehr, stärkstes passendes WLAN wird gewählt, Payload unverändert |
| GPS-Timeout | `AT+FTIME` funktioniert wieder (vorher ca. 26 Tage statt 180 s) |
| Sleep per Taste | Sensoren und Funkchip werden vor dem Deep Sleep wieder abgeschaltet |
| Buzzer | lässt sich mit Core 3.x wieder sauber abschalten |
| Robustheit | fehlendes `return` in `GPS_DATA()`, GPS-Logpuffer für TrackerD-LS (17 Bytes), Begrenzung der seriellen Eingabe, Downlink-Datenrate (`22 00 dr`) |

Die Änderungen sind einzeln in der Git-History nachvollziehbar (`git log` auf Branch `SurfCam`).

## Hardware

| Funktion | ESP32-GPIO |
|---|---|
| GNSS UART (Modul TX → ESP RX / ESP TX → Modul RX) | IO9 / IO10, 9600 Baud |
| GNSS Versorgung (HIGH = an) / Reset (HIGH = Reset) | IO12 / IO25 |
| LoRa SX1276 NSS, SCK, MISO, MOSI, RST | 18, 5, 19, 27, 23 |
| LoRa DIO0, DIO1, DIO2 | 26, 33, 32 |
| I²C SDA / SCL (LIS3DH, GXHT30) | 21 / 22 |
| LIS3DH Interrupt (Wake-up) | IO14 |
| Taster | IO0, active-low (TrackerD); IO25, active-high (TrackerD-LS, andere Platine – auf v1.2 ist IO25 der GPS-Reset) |
| LED rot / blau / grün | IO13 / IO2 / IO15 (beim TrackerD-LS sind rot und blau vertauscht) |
| Buzzer | IO4 |
| Akkuspannung | IO35 (TrackerD-LS: IO34), Teiler 100k/470k |

GNSS-Modul laut Dragino: **Quectel L70 oder L76K**, je nach Charge. Die Firmware wertet `$GNGSA` aus, was zum L76K passt. Welches Modul im eigenen Gerät steckt, ist nicht dokumentiert. Die Ausgabe mit `AT+SHOWID=1` hilft beim Prüfen.

Die Pins IO9/IO10 sind nur im Flash-Modus **DIO** frei. Die Board-Einstellung darf deshalb nicht auf QIO stehen.

## Build

### Arduino IDE (verwendet)

1. Boardverwaltung: **esp32 by Espressif Systems, Version 3.x**
2. Board: **ESP32 PICO-D4** (`pico32`), Flash Mode **DIO**, Upload Speed 921600
3. Bibliothek: **MCCI LoRaWAN LMIC library ≥ 5.0** über den Bibliotheksverwalter installieren
4. Region in `…/libraries/MCCI_LoRaWAN_LMIC_library/project_config/lmic_project_config.h` einstellen, z. B. für die Schweiz/EU:
   ```c
   #define CFG_eu868 1
   #define CFG_sx1276_radio 1
   ```
   Es darf nur eine Region aktiv sein.
5. `TrackerD.ino` öffnen und hochladen. TinyGPS++, OneButton und der LIS3DH-Treiber liegen bereits im Sketch-Ordner.

> Die Kopie der LMIC-Bibliothek unter `Library/arduino-lmic` ist Version 4.1.0 mit AU915-Konfiguration und passt **nicht** zu dieser Firmware.

### PlatformIO

`Example/LoRaWAN/platformio.ini` ist noch auf Core 2.0.1 und LMIC 4.1.1 eingestellt und baut diese Firmware **nicht**. Vor einer Nutzung müssen `platform`/`platform_packages` auf Core 3.x und `lib_deps` auf LMIC ≥ 5.0 angepasst werden.

## Inbetriebnahme

1. Gerät per USB-C anschliessen, seriellen Monitor mit 115200 Baud öffnen.
2. Reset drücken. Für `AT+ATST` Sekunden (Standard 15) nimmt das Gerät AT-Befehle an.
3. LoRaWAN-Schlüssel prüfen bzw. setzen: `AT+DEUI=?`, `AT+APPEUI=…`, `AT+APPKEY=…`
4. Modus wählen, z. B. `AT+SMOD=1,0,0` (GPS) und `AT+TDC=300000` (5 min)
5. `ATZ` für einen Neustart. Das Gerät joint per OTAA und sendet anschliessend.

Mit `AT+CFG` lassen sich alle Einstellungen auf einmal anzeigen.

## Betriebsmodi

| Modus | Ablauf |
|---|---|
| GPS (`AT+SMOD=1,x,x`) | Aufwachen → GPS einschalten → auf Fix warten (max. `AT+FTIME`) → Uplink → Deep Sleep für `AT+TDC` |
| WLAN (`AT+SMOD=2,0,2`) | Aufwachen → WLAN-Scan → Uplink mit stärkstem (passendem) WLAN → Deep Sleep |
| Hybrid (`AT+SMOD=3,0,2`) | WLAN-Scan; findet er kein passendes WLAN, folgt eine GPS-Messung |
| GPS/WLAN abwechselnd (`AT+BG=1`) | jeder GPS-Uplink wird von einem WLAN-Uplink gefolgt |
| Bewegung (`AT+INTWK=1`) | bei Bewegung wird im Intervall `AT+MTDC` gesendet, in Ruhe im Intervall `AT+TDC` |
| Alarm | per langem Tastendruck oder Sturzerkennung; Uplinks im Intervall `AT+ATDC`, bis zu 60 Mal oder bis Downlink `02 01` |

## Uplinks

Alle Werte Big Endian. Breite/Länge sind `int32` in Grad × 10⁶, Batterie in mV. Das Alarm-Byte enthält `alarm << 6 | bat >> 8`, gefolgt vom unteren Batterie-Byte.

| Port | Inhalt |
|---|---|
| 2 | GPS (`b = 0`): Lat 4, Lon 4, Alarm/Bat 2, Flags 1, dann Feuchte 2 + Temperatur 2 (je × 10) **oder** bei `AT+BG=1` GPS-Zeit 7 (Jahr 2, Mon, Tag, h, min, s) |
| 3 | GPS (`b = 1`): Lat 4, Lon 4, Alarm/Bat 2, Flags 1 [+ GPS-Zeit 7] |
| 4 | Nachgesendete GPS-Logdaten (`AT+PNACKMD=1`): Lat 4, Lon 4, GPS-Zeit 7 [+ Bat 2 bei TrackerD-LS] |
| 5 | Gerätestatus: Typ 1, Firmware 2, Band 1, Sub-Band 1, Bat 2, SMOD 1, Flag 1 |
| 7 | Alarm: Alarm/Bat 2, Flags 1 |
| 8 | WLAN: SSID 6 (letzte 6 Zeichen, kürzere mit `c` aufgefüllt, `ffffff` = nichts gefunden), RSSI 1 (`int8`, 0 = nichts gefunden), Alarm/Bat 2, Flags 1 |
| 10 | TTN Mapper (`b = 2`): Lat 4, Lon 4, Höhe 2 (m), HDOP 1 (× 10), Alarm/Bat 2, Flags 1 |

Flags bei Port 2/3/10: `mod << 6 | lon << 5 | intwk << 4 | bg << 3`; bei Port 8: `2 << 6 | lon << 5 | intwk << 4`.

## LEDs

| Anzeige | Bedeutung |
|---|---|
| Blau, Rot, Grün nacheinander | Kaltstart (Power-on / Reset) |
| Blau kurz | LoRaWAN-Join läuft |
| Grün | Join erfolgreich / Uplink wird gesendet / Downlink empfangen |
| Blau schnell blinkend | GPS sucht |
| Blau 1 s | GPS-Fix |
| Rot 1 s | Uplink ohne gültige Position bzw. Alarm |

Mit `AT+LON=0` sind die LEDs aus.

## Bekannte Einschränkungen

- **Nicht auf Hardware getestet:** Die Änderungen dieses Forks sind am Code geprüft, aber noch nicht auf einem Gerät verifiziert.
- `AT+SLEEP` beim TrackerD (Typ 13) schaltet den Funkchip nicht ab (Verhalten aus dem Original).
- Die IMU-Initialisierung für den Schrittzähler (`AT+PM=1`) ist in `setup()` auskommentiert. Ohne `AT+INTWK=1` wird der LIS3DH dann nicht konfiguriert.
- Weitere bekannte Fehler aus dem Original, noch nicht behoben: `sys.gps_start == 0;` (Vergleich statt Zuweisung) in `print_wakeup_reason()`, verodertes `digitalWrite` der LED-Pins in `onEvent()`, `tdc * 1000` ohne 64 Bit in `alarm_state()`, HDOP-Rundung im TTN-Mapper-Modus.
- `blescan.cpp` wird mitkompiliert, aber nicht aufgerufen.
- `AT+NMEA353` und `AT+NMEA886` werden gespeichert, haben aber keine Wirkung.

## Lizenz

Wie das Dragino-Original (siehe Repository-Wurzel). Hardware-Unterlagen: CC BY-SA 3.0.
