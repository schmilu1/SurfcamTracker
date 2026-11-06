# SurfCam Test-GUI

Webserver auf dem Pi zum Testen: zeigt die Funkpakete des Rovers live an und stellt den Tracker ein, wenn er per USB am Pi steckt.

**Aufruf:** `http://mildersurf.local:8080` oder `http://<IP des Pi>:8080`, im Browser auf dem Pi oder auf dem Handy im selben WLAN. Auf dem Pi selbst geht auch `http://localhost:8080`.

| Tab | Inhalt |
|---|---|
| Live | letztes Paket (Geschwindigkeit, Fix, Bewegung, RSSI/SNR, Pakete/s, Verlust, Akku), Karte mit Spur der letzten 5 min, Verläufe der letzten 2 min, empfangene Pakete als CSV |
| Tracker | nur mit USB: Status, alle Einstellungen (Funkband inkl. Gateway, GNSS-Rate, Sendeabstand, Leistung, Frequenz, SF, Bandbreite, ID, Lograte, Bewegung loggen, LEDs), Neustart, Ausschalten, Werkseinstellungen, Konsole für beliebige AT-Befehle |
| Log | nur mit USB: Sessions im Flash des Trackers, CSV herunterladen, löschen |
| Gateway | aktives Funkband, Zustand des Paket-Forwarders, Band umschalten |

Die Karte braucht Internet (OpenStreetMap). Ohne Internet funktioniert alles andere weiter.

**Hinweise:**

- Beim Einstecken in den Pi startet der Tracker neu (Auto-Reset-Schaltung des USB-Wandlers). Das Log im Flash bleibt erhalten.
- Der Log-Download läuft über USB mit ≈ 100 Positionen/s: eine Stunde bei 5 Hz dauert ≈ 3 min. Währenddessen sind keine anderen Befehle möglich.
- Ohne Anmeldung: Wer im WLAN ist, kann den Tracker einstellen. Für ein offenes WLAN nicht geeignet.

## Aufbau

```
Gateway Bridge ─MQTT─► server.py ◄─USB (AT-Befehle, AT+JSON)─ Tracker
                          │
                          └─HTTP 8080 (Seite, JSON-API, Server-Sent Events)─► Browser
```

| Datei | Inhalt |
|---|---|
| [server.py](server.py) | HTTP-Server, Tracker-Verbindung (pyserial), Status alle 2 s per `AT+JSON` |
| [uplink.py](uplink.py) | MQTT-Client (nur Abonnieren), Protobuf-Leser, Decoder für Rover-Pakete Version 1 und 2 |
| [static/index.html](static/index.html) | Weboberfläche (ohne Build, Leaflet für die Karte) |
| [surfcam-testgui.service](surfcam-testgui.service) | systemd-Dienst |
| [sudoers-surfcam-testgui](sudoers-surfcam-testgui) | erlaubt `systemctl restart lora-pkt-fwd` ohne Passwort (für die Band-Umschaltung) |

Nur Python 3 und pyserial (auf dem Pi vorinstalliert).

## Installation auf dem Pi

```
cd ~ && git clone https://github.com/schmilu1/SurfcamTracker.git    # falls noch nicht vorhanden
sudo install -m 440 ~/SurfcamTracker/pi/testgui/sudoers-surfcam-testgui /etc/sudoers.d/surfcam-testgui
sudo visudo -cf /etc/sudoers.d/surfcam-testgui
sudo cp ~/SurfcamTracker/pi/testgui/surfcam-testgui.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now surfcam-testgui
```

Aktualisieren: `cd ~/SurfcamTracker && git pull && sudo systemctl restart surfcam-testgui`

Log des Dienstes: `journalctl -u surfcam-testgui -f`

## Auf dem Mac testen

Mit dem Tracker am Mac und MQTT vom Pi:

```
python3 server.py --port 8090 --mqtt mildersurf.local --tracker /dev/cu.usbserial-XXXX
```

`http://localhost:8090/#nosse` lädt die Daten einmal ohne Live-Verbindung (für Screenshots), `#tab=tracker` öffnet einen Tab direkt.

## API

| Methode | Pfad | |
|---|---|---|
| GET | `/api/state?seconds=600` | Tracker, MQTT, Gateway, Pakete der letzten n s, Konsole |
| GET | `/api/events` | Server-Sent Events: `packet`, `tracker`, `console`, `mqtt`, `gateway`, `logdump` |
| GET | `/api/packets.csv` | alle empfangenen Pakete (max. 5000) |
| POST | `/api/tracker/cmd` `{"cmd": "AT+..."}` | AT-Befehl, Antwort bis `OK`/`ERROR` |
| POST | `/api/tracker/status` | `AT+JSON` sofort lesen |
| GET | `/api/tracker/logs` | Sessions (`AT+LOG`) |
| GET | `/api/tracker/logs/<session>.csv` | Session herunterladen (`AT+LOGDUMP`, gestreamt) |
| GET | `/api/gateway` | Funkband und Zustand des Forwarders |
| POST | `/api/gateway/band` `{"band": "A", "tracker": true, "gateway": true}` | Band umschalten (Tracker und/oder Gateway) |
