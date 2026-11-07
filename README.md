# SurfCam

Eine feste Kamera am Strand verfolgt einen Surfer anhand seiner GNSS-Position und zeichnet ihn auf. Der Surfer trägt einen umgebauten **Dragino TrackerD**, der seine Position per LoRa an ein Gateway am Strand funkt. Ein Raspberry Pi berechnet daraus den Bildausschnitt im 4K-Video.

```
TrackerD (Rover)                 Raspberry Pi 5 am Strand
L76K GNSS, 5 Hz                  SX1302-HAT ─► lora_pkt_fwd ─► ChirpStack Gateway Bridge ─► MQTT
  │ LoRa P2P, 869.525 MHz           │                                                       │
  └──── SF7 / 250 kHz, ~3 Pakete/s ─┘                                    SurfCam-Dienst ◄───┘
                                                                         (Filter, Geometrie,
                                                                          Bildausschnitt, Video)
```

## Inhalt

| Ordner | Inhalt |
|---|---|
| [firmware/rover](firmware/rover) | Firmware für den TrackerD: GNSS lesen, Positionen im Flash loggen, per LoRa senden. Mit Unit-Tests und AT-Befehlen. **Funkpaketformat in der README.** |
| [firmware/l76k_probe](firmware/l76k_probe) | Test-Sketch: erkennt das GNSS-Modul und misst die erreichbare Update-Rate |
| [docs/pool.md](docs/pool.md) | **Surf-Pool-Modus:** 4K-Stream vom iPhone auf dem Pi puffern, letzte x s als Clip speichern |
| [docs/gateway.md](docs/gateway.md) | Kanalplan und Konfiguration des Gateways (Waveshare SX1302 HAT), Empfang prüfen |
| [docs/hardware](docs/hardware) | Schaltplan, Layout und Bestückungsplan TrackerD v1.2 (Dragino) |
| [pi/testgui](pi/testgui) | **Test-GUI auf dem Pi** (Port 8080): Live-Daten, Tracker per USB einstellen, Log herunterladen, Funkband umschalten |
| [pi/hotspot](pi/hotspot) | **Hotspot auf dem Pi** neben dem eigenen WLAN: GUI auch ohne Heimnetz unter `http://192.168.4.1:8080` |
| [tools/dgps](tools/dgps) | **DGPS-Test:** Tracker mit dem Pi-GPS korrigieren, Auswertung roh gegen korrigiert |
| [tools/rangetest](tools/rangetest) | Reichweitentest auf dem Pi: Paketverlust, RSSI, SNR, Entfernung, CSV |
| [tools/openocd](tools/openocd) | OpenOCD-Konfiguration für JTAG-Debugging über einen FTDI-Adapter |

Der Raspberry-Pi-Dienst (Filter, Geometrie, Bildausschnitt, Ringpuffer) folgt.

## Schnellstart Rover

Voraussetzung: [PlatformIO](https://platformio.org/) (VS-Code-Erweiterung oder CLI).

```
cd firmware/rover
pio run -e trackerd -t upload     # bauen und flashen
pio device monitor                # AT-Befehle, z. B. ATI
pio test -e native                # Unit-Tests ohne Hardware
```

Details, Bedienung, AT-Befehle und Messergebnisse: [firmware/rover/README.md](firmware/rover/README.md).

## Hardware

| Gerät | Komponenten |
|---|---|
| Rover | Dragino TrackerD: ESP32-PICO-D4, SX1276, Quectel L76K (Firmware `URANUS5`), LIS3DH, Li-Ion-Akku |
| Gateway | Raspberry Pi 5, Waveshare SX1302 LoRaWAN Gateway HAT, `lora_pkt_fwd`, ChirpStack Gateway Bridge und Mosquitto in Docker (Konfiguration: [schmilu1/sx1302_hal_rpi5-master](https://github.com/schmilu1/sx1302_hal_rpi5-master), Ordner `surfcam/`) |

## Stand

- Rover-Firmware 0.4 läuft auf dem Gerät: GNSS 10 Hz, Funk Band A (3,3 Pakete/s) oder B (10 Pakete/s), Bewegungssensor, Log als Ringpuffer. Empfang über das Gateway bestätigt.
- Test-GUI auf dem Pi: `http://mildersurf.local:8080`, im Hotspot des Pi `http://192.168.4.1:8080`.
- Offen: Reichweitentest am Strand, Raspberry-Pi-Dienst, Video-Ringpuffer.

## Herkunft und Lizenz

Dieses Repository ist aus [dragino/TrackerD](https://github.com/dragino/TrackerD) entstanden. Die ursprüngliche Dragino-Firmware und -Bibliotheken wurden entfernt, sie sind in der Git-History bis Commit `d365c55` enthalten. Die Hardware-Unterlagen unter [docs/hardware](docs/hardware) stammen von Dragino und stehen unter [CC BY-SA 3.0](http://creativecommons.org/licenses/by-sa/3.0/).
