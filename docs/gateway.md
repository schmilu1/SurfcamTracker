# Gateway: Waveshare SX1302 HAT am Raspberry Pi 5

Empfang der Rover-Pakete (LoRa Punkt-zu-Punkt, Band A 869,525 MHz oder Band B 869,85 MHz, SF7, 250 kHz) über das LoRaWAN-Gateway-HAT. Die Konfiguration ist versioniert im Repo [schmilu1/sx1302_hal_rpi5-master](https://github.com/schmilu1/sx1302_hal_rpi5-master) (privat), Ordner `surfcam/`.

## Aufbau auf dem Pi (`MilderSurf`, Debian 12, aarch64)

```
SX1302 HAT ─SPI─► lora_pkt_fwd ─UDP 1700─► ChirpStack Gateway Bridge ─► Mosquitto (MQTT 1883)
                  (systemd)                 (Docker)                     (Docker)
```

| Komponente | Ort |
|---|---|
| Repo (HAL + Konfiguration) | `~/sx1302_hal_rpi5-master`, Remote `LoRa` |
| Paket-Forwarder | `~/sx1302_hal_rpi5-master/packet_forwarder/lora_pkt_fwd`, systemd-Dienst `lora-pkt-fwd` |
| Forwarder-Konfiguration | `~/sx1302_hal_rpi5-master/surfcam/global_conf_A.json` / `_B.json`, aktiv über den Symlink `global_conf_active.json` |
| Band umschalten | `~/sx1302_hal_rpi5-master/surfcam/band.sh A\|B` (Rover: `AT+BAND=A\|B`) |
| Reichweitentest | `~/SurfcamTracker/tools/rangetest/rangetest.py` ([README](../tools/rangetest/README.md)) |
| Docker | Compose-Projekt `surfcam` (`surfcam/docker-compose.yml`): Gateway Bridge, Mosquitto |
| MQTT | Mosquitto, Port 1883, anonym |
| Gateway-ID | `0016c001f160f1e3` |

Seit dem 05.11.2026 läuft **kein ChirpStack** mehr (Netzwerkserver, Postgres, Redis und Portainer wurden entfernt, ebenso der Dienst `gateway-gps`). Das Gateway leitet nur noch Funkpakete an MQTT weiter.

Der Pi hat keinen GitHub-Zugang. Änderungen werden auf dem Mac committet und gepusht und per `git bundle` über SSH übertragen (oder nach Einrichten eines Zugangs per `git pull LoRa main`).

## GPS des HAT (Kamerastandort)

Das HAT hat ein eigenes GNSS-Modul. Es liefert den Kamerastandort und dient als Referenz für das Differenz-GPS im Positionsbereich (siehe [tools/dgps](../tools/dgps/README.md)). **Entscheidung (06.11.2026):** Der SurfCam-Dienst (vorerst das [Test-GUI](../pi/testgui/README.md)) liest die Schnittstelle selbst, `lora_pkt_fwd` bekommt keinen `gps_tty_path`. Sonst würde der Forwarder die Schnittstelle exklusiv belegen.

**Modul (geprüft 06.11.2026):** Kein MediaTek-L76B, wie früher hier stand. Es antwortet nicht auf `PMTK`-Befehle, meldet `$GPTXT,01,01,01,ANTENNA OK` und versteht die CASIC-Befehle des L76K im Tracker (`PCAS04,7` schaltet BeiDou zu). Gleiche Chipfamilie wie im Tracker ist günstig für die Differenzkorrektur.

| | |
|---|---|
| Schnittstelle | **`/dev/ttyAMA0`**, nach dem Einschalten 9600 Baud, NMEA (`$GN…`, GPS + GLONASS). Nicht `/dev/serial0`, das zeigt auf dem Pi 5 auf die Debug-Schnittstelle `ttyAMA10` |
| Einstellung durch das Test-GUI | bei jedem Start dieselben Satellitensysteme wie der Tracker: GPS + BeiDou + GLONASS (`PCAS04,7`), nur RMC + GGA (`PCAS03`). 1 Hz bei 9600 Baud. Nicht im Modul gespeichert |
| **10 Hz: Modul hängt** | Test 06.11.2026: nach `PCAS01,5` (115200 Baud) und `PCAS02,100` (10 Hz) kamen noch einzelne Positionen, dann blieb die Sendeleitung auf Low. Kein Befehl half mehr, nur ein Stromunterbruch (Pi ausschalten und Netzteil ziehen). Der L76K im Tracker läuft mit 10 Hz. Für die Differenzkorrektur reicht 1 Hz, weil sich der Fehler über Minuten ändert |
| Rechte | Benutzer `Milder` ist in `dialout` |
| Test 06.11.2026 | Daten kommen, `ANTENNA OK`, aber 0 Satelliten (Antenne im Haus) |

## Kanalplan Band A (geändert am 05.11.2026)

Band B (seit 06.11.2026): Funkteil 1 auf 869,2 MHz, LoRa-Einzelkanal 869,85 MHz (IF +650 kHz), 868,3 MHz auf Funkteil 0. Details im Gateway-Repo, `surfcam/README.md`.

Ein Funkteil des SX1302 empfängt Kanäle bis `|IF| + Bandbreite/2 ≤ 800 kHz` um seine Mittenfrequenz (`LGW_RF_RX_BANDWIDTH_* = 1600000` in `loragw_hal.c`). Damit der 250-kHz-Einzelkanal auf 869,525 MHz Platz hat, wurden die Mittenfrequenzen verschoben. Alle 8 LoRaWAN-Kanäle sind weiterhin konfiguriert.

| Kanal | vorher | nachher |
|---|---|---|
| Funkteil 0 (Mitte) | 867,5 MHz | **867,6 MHz** |
| Funkteil 1 (Mitte) | 868,5 MHz | **868,9 MHz** |
| multiSF 0–4 | 867,1 / 867,3 / 867,5 / 867,7 / 867,9 (Funkteil 0) | gleich (Funkteil 0) |
| multiSF 5 | – | 868,1 (Funkteil 0) |
| multiSF 6–7 | 868,1–868,5 auf Funkteil 1 | 868,3 / 868,5 (Funkteil 1) |
| LoRa-Einzelkanal 250 kHz, SF7 | 868,3 MHz | **869,525 MHz** (IF +625 kHz) |
| FSK 50 kbit/s | 868,8 MHz | 868,8 MHz |

Nach Änderungen an `global_conf_*.json`: `sudo systemctl restart lora-pkt-fwd`.

## Empfang prüfen

Die Ausgabe von `lora_pkt_fwd` im Journal ist gepuffert und erscheint verzögert. Zuverlässiger ist MQTT:

```
docker exec surfcam-mosquitto-1 mosquitto_sub -t "eu868/gateway/+/event/up" -F "%I %t %x"
```

Die Gateway Bridge veröffentlicht Protobuf (`UplinkFrame`). Rover-Pakete beginnen mit `0a 16 e0 4x`: Feld 1 (`phy_payload`), 22 Bytes, MHDR `0xE0`, Version 2 und Geräte-ID im unteren Teil des zweiten Bytes (bis Rover 0.3.0: `0a 13 e0 2x`, 19 Bytes, Version 1).

Feldnummern, die der Pi-Dienst braucht:

| Feld | Inhalt |
|---|---|
| 1 | `phy_payload` (Rover-Paket, siehe [firmware/rover/README.md](../firmware/rover/README.md)) |
| 4.1 | Frequenz in Hz |
| 4.2.3 | LoRa-Modulation: 1 Bandbreite (Hz), 2 SF, 5 Coding Rate |
| 5.1 | Gateway-ID (Text) |
| 5.6 | RSSI (int32, dBm) |
| 5.7 | SNR (float, dB) |

## Empfangstests (05.11.2026, Rover im Haus, wenige Meter vom Gateway)

```
Gateway 0016c001f160f1e3, Frequenz 869.525 MHz, Bandbreite 250 kHz, SF7, RSSI -22 dBm, SNR 14.2 dB
PHYPayload e0 21 b5 a4 c6 de b0 00 … 00 8d
Version 1, ID 1, Seq 181, Akku 3800 mV, Zeit 22:15:42.000 UTC, kein Fix, HDOP 14.1
```

- ohne Fix: 11 Pakete in 22 s (Lebenszeichen alle 2 s), Sequenz lückenlos
- mit Fix (nach Umstellung auf das Compose-Projekt `surfcam`): 50 Pakete in 15 s (≈ 3,3 Pakete/s), Sequenz lückenlos

Nebenbei empfängt das Gateway LoRaWAN-Pakete fremder Geräte in der Umgebung (`0x40`, Unconfirmed Data Up, auf 868,3/868,5 MHz). Der Pi-Dienst filtert nach MHDR `0xE0` und Version.
