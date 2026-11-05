# Gateway: Waveshare SX1302 HAT am Raspberry Pi 5

Empfang der Rover-Pakete (LoRa Punkt-zu-Punkt, 869,525 MHz, SF7, 250 kHz) über das vorhandene LoRaWAN-Gateway, parallel zum normalen LoRaWAN-Betrieb.

## Aufbau auf dem Pi (`MilderSurf`, Debian 12, aarch64)

| Komponente | Ort |
|---|---|
| Paket-Forwarder | `lora_pkt_fwd` aus `~/Documents/LoRaModul/sx1302_hal_rpi5-master`, systemd-Dienst `lora-pkt-fwd` |
| Konfiguration | `~/lorawan/configuration/global_conf.json` |
| ChirpStack v4 | Docker-Compose-Projekt `~/lorawan` (chirpstack, gateway-bridge, mosquitto, postgres, redis) |
| MQTT | Mosquitto, Port 1883, anonym |
| Gateway-ID | `0016c001f160f1e3` |

## Kanalplan (geändert am 05.11.2026)

Ein Funkteil des SX1302 empfängt Kanäle bis `|IF| + Bandbreite/2 ≤ 800 kHz` um seine Mittenfrequenz (`LGW_RF_RX_BANDWIDTH_* = 1600000` in `loragw_hal.c`). Damit der 250-kHz-Einzelkanal auf 869,525 MHz Platz hat, wurden die Mittenfrequenzen verschoben. Alle 8 LoRaWAN-Kanäle bleiben erhalten.

| Kanal | vorher | nachher |
|---|---|---|
| Funkteil 0 (Mitte) | 867,5 MHz | **867,6 MHz** |
| Funkteil 1 (Mitte) | 868,5 MHz | **868,9 MHz** |
| multiSF 0–4 | 867,1 / 867,3 / 867,5 / 867,7 / 867,9 (Funkteil 0) | gleich (Funkteil 0) |
| multiSF 5 | – | 868,1 (Funkteil 0) |
| multiSF 6–7 | 868,1–868,5 auf Funkteil 1 | 868,3 / 868,5 (Funkteil 1) |
| LoRa-Einzelkanal 250 kHz, SF7 | 868,3 MHz | **869,525 MHz** (IF +625 kHz) |
| FSK 50 kbit/s | 868,8 MHz | 868,8 MHz |

Sicherung der alten Konfiguration: `~/lorawan/configuration/global_conf.json.bak-20261105-vor-surfcam`

Zurücksetzen:

```
cd ~/lorawan/configuration
cp global_conf.json.bak-20261105-vor-surfcam global_conf.json
sudo systemctl restart lora-pkt-fwd
```

## Empfang prüfen

Die Ausgabe von `lora_pkt_fwd` im Journal ist gepuffert und erscheint verzögert. Zuverlässiger ist MQTT:

```
docker exec lorawan-mosquitto-1 mosquitto_sub -t "eu868/gateway/+/event/up" -F "%I %t %x"
```

Die Gateway Bridge veröffentlicht Protobuf (`UplinkFrame`). Rover-Pakete beginnen mit `0a 13 e0 2x`: Feld 1 (`phy_payload`), 19 Bytes, MHDR `0xE0`, Version 1 und Geräte-ID im unteren Teil des zweiten Bytes.

Feldnummern, die der Pi-Dienst braucht:

| Feld | Inhalt |
|---|---|
| 1 | `phy_payload` (Rover-Paket, siehe [firmware/rover/README.md](../firmware/rover/README.md)) |
| 4.1 | Frequenz in Hz |
| 4.2.3 | LoRa-Modulation: 1 Bandbreite (Hz), 2 SF, 5 Coding Rate |
| 5.1 | Gateway-ID (Text) |
| 5.6 | RSSI (int32, dBm) |
| 5.7 | SNR (float, dB) |

## Erster Empfangstest (05.11.2026, Rover im Haus, wenige Meter vom Gateway)

```
Gateway 0016c001f160f1e3, Frequenz 869.525 MHz, Bandbreite 250 kHz, SF7, RSSI -22 dBm, SNR 14.2 dB
PHYPayload e0 21 b5 a4 c6 de b0 00 … 00 8d
Version 1, ID 1, Seq 181, Akku 3800 mV, Zeit 22:15:42.000 UTC, kein Fix, HDOP 14.1
```

11 Pakete in 22 s (Lebenszeichen alle 2 s ohne Fix), Sequenznummern lückenlos.

Nebenbei empfängt das Gateway LoRaWAN-Pakete fremder Geräte in der Umgebung (`0x40`, Unconfirmed Data Up, auf 868,3/868,5 MHz). Der Pi-Dienst filtert nach MHDR `0xE0` und Version.
