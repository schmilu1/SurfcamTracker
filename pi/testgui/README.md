# SurfCam Test-GUI

Webserver auf dem Pi zum Testen: zeigt die Funkpakete des Rovers live an und stellt den Tracker ein, wenn er per USB am Pi steckt.

**Aufruf:** `http://mildersurf.local:8080` oder `http://<IP des Pi>:8080`, im Browser auf dem Pi oder auf dem Handy im selben WLAN. Auf dem Pi selbst geht auch `http://localhost:8080`. Im [Hotspot des Pi](../hotspot/) (ohne Heimnetz): `http://192.168.4.1:8080`.

| Tab | Inhalt |
|---|---|
| Position | Tracker relativ zur Kamera (Entfernung, Richtung) roh und mit dem Pi-GPS korrigiert, Kamerastandort festlegen (Tracker neben den Pi legen), Vergleich roh/korrigiert mit Streudiagramm, Status und Mittelwert des Pi-GPS, Aufzeichnungen herunterladen. Siehe [DGPS-Test](../../tools/dgps/README.md) |
| Live | letztes Paket (Geschwindigkeit, Fix, Bewegung, RSSI/SNR, Pakete/s, Verlust, Akku), Karte mit Spur der letzten 5 min, Verläufe der letzten 2 min, empfangene Pakete als CSV |
| Tracker | nur mit USB: Status; Einstellungen als Formular (mehrere Werte auf einmal, Funkband inkl. Gateway); **Befehle**: jeder AT-Befehl als Button, mit Eingabefeld und Hinweis wo ein Wert nötig ist, „Abfragen“ zeigt den aktuellen Wert, Felder sind mit den aktuellen Werten vorbelegt, Sessions zum Herunterladen/Löschen als Auswahl; Konsole mit der vollständigen Ausgabe (eigener Befehl nur noch aufklappbar) |
| Log | nur mit USB: Sessions im Flash des Trackers, CSV herunterladen, löschen |
| Gateway | aktives Funkband, Zustand des Paket-Forwarders, Band umschalten |
| Hilfe | diese README und die READMEs zu Hotspot, Tracker-Firmware und Gateway, direkt aus dem Repo auf dem Pi (nach `git pull` aktuell, ohne Internet lesbar) |

Die Karte braucht Internet (OpenStreetMap). Ohne Internet funktioniert alles andere weiter.

**Hinweise:**

- Beim Einstecken in den Pi startet der Tracker neu (Auto-Reset-Schaltung des USB-Wandlers). Das Log im Flash bleibt erhalten.
- Der Log-Download läuft über USB mit ≈ 100 Positionen/s: eine Stunde bei 5 Hz dauert ≈ 3 min. Währenddessen sind keine anderen Befehle möglich.
- iPhone, Symbol auf dem Home-Bildschirm: beim Hinzufügen «Als Web-App öffnen» ausschalten. Sonst bricht die Web-App mit «HTTPS-only» ab, weil das GUI nur HTTP spricht; als Lesezeichen öffnet Safari die Seite nach einer Warnung («Website trotzdem besuchen»).
- Ohne Anmeldung: Wer im WLAN oder im Hotspot ist, kann den Tracker einstellen. Den Hotspot deshalb nur mit Passwort betreiben.

## Aufbau

```
Gateway Bridge ─MQTT─► server.py ◄─USB (AT-Befehle, AT+JSON)─ Tracker
                          │
                          └─HTTP 8080 (Seite, JSON-API, Server-Sent Events)─► Browser
```

| Datei | Inhalt |
|---|---|
| [server.py](server.py) | HTTP-Server, Tracker-Verbindung (pyserial), Status alle 2 s per `AT+JSON` |
| [position.py](position.py) | GPS des Pi (HAT, `/dev/ttyAMA0`) lesen und wie den Tracker einstellen, Aufzeichnung, Kamerastandort, Differenzkorrektur |
| [uplink.py](uplink.py) | MQTT-Client (nur Abonnieren), Protobuf-Leser, Decoder für Rover-Pakete Version 1 und 2 |
| [static/index.html](static/index.html) | Weboberfläche (ohne Build, Leaflet für die Karte) |
| [surfcam-testgui.service](surfcam-testgui.service) | systemd-Dienst |
| [sudoers-surfcam-testgui](sudoers-surfcam-testgui) | erlaubt `systemctl restart lora-pkt-fwd` ohne Passwort (für die Band-Umschaltung) |

Nur Python 3 und pyserial (auf dem Pi vorinstalliert).

Aufzeichnungen (Pi-GPS, empfangene Pakete) und der Kamerastandort liegen in `~/surfcam-data/` (Option `--data`). Das GPS des Pi liest der Server von `/dev/ttyAMA0` (`--pi-gnss`, `aus` zum Abschalten) mit 10 Hz (`--pi-gnss-rate`).

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
| GET | `/api/position?window=300` | Pi-GPS, Kamerastandort, letzte Relativposition, Statistik und Punkte der letzten n s |
| POST | `/api/position/calibrate` `{"seconds": 60}` | Kamerastandort festlegen (Tracker neben dem Pi) |
| POST | `/api/position/cancel`, `/clear_cam`, `/reset_base` | Kalibrierung abbrechen, Kamerastandort löschen, Pi-Mittelwert neu beginnen |
| GET | `/api/data` | Aufzeichnungen (`rx`, `pi_gnss`) |
| GET | `/api/data/<rx\|pi_gnss>/<datei>.csv` | Aufzeichnung herunterladen |
| GET | `/api/help` | README-Dateien für den Tab Hilfe (Markdown) |
| POST | `/api/gateway/band` `{"band": "A", "tracker": true, "gateway": true}` | Band umschalten (Tracker und/oder Gateway) |
