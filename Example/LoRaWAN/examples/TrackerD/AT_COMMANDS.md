# AT-Befehle – TrackerD-Firmware

Referenz für die AT-Befehle dieser Firmware (Stand: Fork `SurfCam`, Firmware-Kennung `TrackerD v1.4.9`).
Die Angaben stammen aus dem Quellcode ([at.h](at.h), [at.cpp](at.cpp), [TrackerD.ino](TrackerD.ino)) und nicht aus den Hilfetexten in `at.h`. Diese sind teilweise kopiert und falsch.

## Verbindung

| Einstellung | Wert |
|---|---|
| Schnittstelle | USB-C (USB-Seriell-Wandler auf der Platine) |
| Baudrate | 115200, 8N1 |
| Zeilenende | `\r\n` oder `\n` (wird abgeschnitten) |
| Max. Länge | 127 Zeichen pro Befehl |

Das Gerät nimmt nur Befehle an, solange es wach ist. Nach einem Reset bleibt das AT-Fenster `AT+ATST` Sekunden offen (Standard 15 s), danach geht das Gerät schlafen. Zum Konfigurieren also kurz vorher einen Reset machen (Taste RST oder `ATZ`).

## Syntax

| Eingabe | Wirkung |
|---|---|
| `AT` | Verbindungstest, Antwort `OK` |
| `AT+XXX=?` | Wert lesen |
| `AT+XXX=<wert>` | Wert setzen; bei Erfolg wird die gesamte Konfiguration im Flash gespeichert |
| `AT+XXX?` | Hilfetext ausgeben |
| `AT+XXX` | Befehl ausführen (nur bei `ATZ`, `AT+FDR`, `AT+SLEEP`, `AT+CFG`) |

Antworten: `OK`, `ERROR` (unbekannter Befehl), `AT_PARAM_ERROR` (ungültiger Wert).

> **Einheiten beachten:** Die Intervalle `AT+TDC`, `AT+MTDC`, `AT+ATDC` und `AT+EAT` sind in **Millisekunden**, `AT+FTIME` und `AT+ATST` in **Sekunden**. Die entsprechenden Downlinks verwenden durchgehend Sekunden.

## Allgemein

| Befehl | Beschreibung |
|---|---|
| `AT+MODEL=?` | Modell und Firmware-Version ausgeben |
| `ATZ` | Konfiguration speichern und Neustart |
| `AT+FDR` | Werkseinstellungen (Konfiguration löschen) und Neustart |
| `AT+SLEEP` | Sofort in den Deep Sleep (Aufwecken nur per Reset). Siehe [Hinweise](#hinweise) |
| `AT+CFG` | Alle Einstellungen ausgeben |
| `AT+DEVICE=<n>` | Gerätetyp: `13` = TrackerD, `22` = TrackerD-LS. Bestimmt LED-Belegung, Taster-Logik und GPS-Log-Format |
| `AT+SHOWID=<0\|1>` | Ausführliche Debug-Ausgabe (GPS-Daten, WLAN-Scan, Interrupts) |
| `AT+ATST=<s>` | Länge des AT-Fensters nach dem Start in Sekunden, min. 15 |

## LoRaWAN

| Befehl | Werte | Beschreibung |
|---|---|---|
| `AT+DEUI=<16 hex>` | z. B. `70B3D57ED0012345` | Device EUI |
| `AT+APPEUI=<16 hex>` | | Application / Join EUI |
| `AT+APPKEY=<32 hex>` | | Application Key (OTAA) |
| `AT+DADDR=<8 hex>` | | Device Address (ABP) |
| `AT+NWKSKEY=<32 hex>` | | Network Session Key (ABP) |
| `AT+APPSKEY=<32 hex>` | | Application Session Key (ABP) |
| `AT+NJM=<0\|1>` | `0` ABP, `1` OTAA (Standard) | Join-Modus; wirkt nach Neustart |
| `AT+ADR=<0\|1>` | `0` aus, `1` an | Adaptive Data Rate |
| `AT+DR=<0..7>` | | Datenrate (bei ADR aus) |
| `AT+TXP=<0..5>` | `0` = max. Leistung | Sendeleistungsstufe |
| `AT+CFM=<0\|1>` | | Bestätigte Uplinks (confirmed) |
| `AT+PNACKMD=<0\|1>` | | Nicht bestätigte GPS-Daten im Flash puffern und später nachsenden; setzt `AT+CFM` mit |
| `AT+CHE=<0..9>` | | Sub-Band (nur US915/AU915) |
| `AT+CHS=<0..88>` | Kanalindex | Einzelkanal-Betrieb, wirkt nur im ABP-Modus (`AT+NJM=0`) |
| `AT+DWELLT=<0\|1>` | | Uplink Dwell Time (nur AS923/AU915) |

Das LoRaWAN-Frequenzband wird beim Kompilieren festgelegt, nicht per AT-Befehl (siehe [README](README.md#build)).

## Betriebsmodus

### `AT+SMOD=a,b,c`

Genau 5 Zeichen, z. B. `AT+SMOD=1,0,0`.

| Feld | Werte | Bedeutung |
|---|---|---|
| `a` – Sensormodus | `1` | nur GPS |
| | `2` | nur WLAN-Ortung |
| | `3` | Hybrid: zuerst WLAN, ohne passendes WLAN dann GPS |
| `b` – Uplink-Format | `0` | GPS + Batterie + Temperatur/Feuchte (Port 2) |
| | `1` | GPS + Batterie, ohne Temperatur/Feuchte (Port 3) |
| | `2` | TTN-Mapper-Format mit Höhe und HDOP (Port 10) |
| `c` – Funkmodus | `2` | WLAN |
| | `0`, `1`, `3` | BLE – **in diesem Build deaktiviert** |

Bei `a = 2` oder `3` ist nur `c = 2` erlaubt, sonst kommt `AT_PARAM_ERROR`. Alte Konfigurationen mit BLE-Modus werden beim Start automatisch auf WLAN umgestellt.

Beispiele:

```
AT+SMOD=1,0,0    GPS mit Temperatur/Feuchte
AT+SMOD=1,2,0    GPS im TTN-Mapper-Format
AT+SMOD=2,0,2    nur WLAN
AT+SMOD=3,0,2    WLAN, sonst GPS
```

### Weitere Modus-Befehle

| Befehl | Werte | Beschreibung |
|---|---|---|
| `AT+BG=<0\|1>` | | GPS/WLAN abwechselnd: nach jedem GPS-Uplink folgt ein WLAN-Uplink. Bei `1` enthält der GPS-Uplink den GPS-Zeitstempel statt Temperatur/Feuchte |
| `AT+INTWK=<0\|1>` | | Bewegungsmodus: bei Bewegung (LIS3DH) häufiger senden (`AT+MTDC`) |
| `AT+PM=<0\|1>` | | Schrittzähler |
| `AT+FD=<0\|1>` | | Sturzerkennung; `1` schaltet `AT+INTWK` aus |
| `AT+PT=<2 hex>` | Standard `14` (= 20) | Bewegungsschwelle des LIS3DH (Register `INT1_THS`), **hexadezimal** |

## Intervalle

| Befehl | Einheit | Standard | Beschreibung |
|---|---|---|---|
| `AT+TDC=<ms>` | ms, min. 60 | 1200000 (20 min) | Normales Sendeintervall |
| `AT+MTDC=<ms>` | ms, min. 60 | 300000 (5 min) | Sendeintervall in Bewegung (`AT+INTWK=1`) |
| `AT+ATDC=<ms>` | ms | 60000 (1 min) | Sendeintervall im Alarmzustand |
| `AT+FTIME=<s>` | s | 180 | Maximale GPS-Suchzeit pro Messung; `0` = ohne Zeitlimit |

## GPS

| Befehl | Werte | Beschreibung |
|---|---|---|
| `AT+PDOP=<n>` | Standard 7 | Maximaler DOP-Wert für einen gültigen Fix. Per AT nur ganze Zahlen; Nachkommastellen nur per Downlink `AD` |
| `AT+GF=<0\|1>` | Standard 1 | `1` GPS im Sleep abschalten; `0` GPS versorgt lassen (schnellerer Fix, höherer Verbrauch) |
| `AT+PDTA=<adresse>` | | Gespeicherte GPS-Logdaten ab Flash-Adresse ausgeben |
| `AT+NMEA353=<0..4>` | | Gespeichert, in dieser Firmware ohne Wirkung |
| `AT+NMEA886=<n>` | | Gespeichert, in dieser Firmware ohne Wirkung |

## WLAN / BLE

| Befehl | Beschreibung |
|---|---|
| `AT+WiFiMASK=<text>` | Filter für WLAN-Namen (max. 10 Zeichen). Gesendet wird das stärkste WLAN, dessen SSID den Text enthält. Ohne Filter (`000000` oder weniger als 6 Zeichen) wird das stärkste WLAN ab −90 dBm genommen |
| `AT+BLEMASK=<text>` | Filter für BLE-Beacons – **ohne Wirkung, BLE ist deaktiviert** |

## LED, Buzzer, Taster

| Befehl | Werte | Beschreibung |
|---|---|---|
| `AT+LON=<0\|1>` | Standard 1 | Status-LEDs ein/aus |
| `AT+BEEP=<0\|1>` | | Buzzer beim Alarm |
| `AT+EAT=<ms>` | Standard 2000 | Mindestdauer des Tastendrucks, um einen Alarm auszulösen |

---

## Downlink-Befehle (LoRaWAN)

Downlinks werden als Hex-Bytes gesendet. Der FPort ist beliebig. Alle Werte sind Big Endian.

| Downlink | Entspricht | Beschreibung |
|---|---|---|
| `01 xx xx xx` | `AT+TDC` | Sendeintervall in **Sekunden** (min. 6) |
| `02 01` | | Alarm beenden |
| `02 02` | | Gerät suchen: Buzzer + rote LED |
| `03 xx xx xx` | `AT+MTDC` | Bewegungs-Intervall in Sekunden (min. 6) |
| `04 FF` | `ATZ` | Neustart |
| `04 FE` | `AT+FDR` | Werkseinstellungen + Neustart |
| `05 0x` | `AT+CFM` | `01` bestätigt, `00` unbestätigt |
| `20 0x` | `AT+NJM` | `01` OTAA, `00` ABP, danach Neustart |
| `22 01` | `AT+ADR=1` | ADR an |
| `22 00 dr yy` | `AT+ADR=0` | ADR aus mit Datenrate `dr`; `yy = FF` startet neu |
| `23 01` | | Gerätestatus-Uplink anfordern (Port 5) |
| `24 xx` | `AT+CHE` | Sub-Band (US915/AU915) |
| `25 xx` | `AT+DWELLT` | Dwell Time (AS923/AU915) |
| `34 xx` | `AT+PNACKMD` | |
| `A5 bb` | | Uplink-Format (`b` von `AT+SMOD`) |
| `A5 aa bb cc` | `AT+SMOD` | Bei `aa = 2/3` nur mit `cc = 02` erlaubt (BLE deaktiviert) |
| `AA xx xx` | `AT+FTIME` | GPS-Suchzeit in Sekunden |
| `AB xx` | `AT+NMEA886` | |
| `AC xx` | `AT+NMEA353` | |
| `AD xx xx` | `AT+PDOP` | DOP × 10, z. B. `AD 00 46` = 7,0 |
| `AE xx` | `AT+LON` | |
| `AF xx` | `AT+INTWK` | |
| `B1 xx xx xx` | `AT+ATDC` | Alarm-Intervall in Sekunden (min. 6) |
| `B2 …` | `AT+BLEMASK` | ohne Wirkung |
| `B3 …` | `AT+WiFiMASK` | bis 10 Bytes; jedes Byte ist `Zeichen − 0x30` (z. B. `'A'` = `0x11`) |
| `B4 xx` | `AT+PT` | |
| `B5 xx` | `AT+ATST` | |
| `B6 xx` | `AT+PM` | |
| `B7 xx` | `AT+FD` | |
| `B8 xx` | `AT+BG` | |
| `B9 xx` | `AT+BEEP` | |
| `BA xx` | `AT+EAT` | in **Sekunden** |

## Hinweise

- **`AT+SLEEP` beim TrackerD (Typ 13):** Wie im Dragino-Original wird `esp_deep_sleep_start()` vor dem Abschalten des Funkchips aufgerufen. Der SX1276 bleibt deshalb im Standby. Beim TrackerD-LS (Typ 22) wird korrekt abgeschaltet.
- **Befehlserkennung:** Ein Befehl wird über einen Teilstring-Vergleich gefunden. Tippfehler mit Zusatzzeichen (z. B. `AT+TDCX=…`) können daher trotzdem einen Befehl treffen.
- **Ungültige Werte:** Bei `AT_PARAM_ERROR` wird nichts gespeichert. Einige ältere Setter (`AT+EAT`, `AT+ATDC`, `AT+FTIME`) prüfen den Bereich jedoch nicht.
