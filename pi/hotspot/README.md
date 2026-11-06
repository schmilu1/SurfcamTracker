# Hotspot auf dem Pi

Der Pi bleibt in deinem WLAN (zum Konfigurieren per SSH) und bietet gleichzeitig einen eigenen Hotspot an. So erreichst du das [Test-GUI](../testgui/) auch ohne dein WLAN, zum Beispiel am Strand.

| | im eigenen WLAN | im Hotspot |
|---|---|---|
| Pi | `mildersurf.local` / DHCP-Adresse | `192.168.4.1` |
| Test-GUI | `http://mildersurf.local:8080` | `http://192.168.4.1:8080` |

## Funktionsweise

Der WLAN-Chip des Pi 5 kann einen Client und einen Hotspot gleichzeitig betreiben, aber nur auf **einem** Kanal. Neben `wlan0` (Client) gibt es deshalb die virtuelle Schnittstelle `uap0` (Hotspot). `surfcam-ap.sh` läuft alle 30 s (`surfcam-ap.timer`) und

- legt `uap0` an, falls sie fehlt (z. B. nach einem Neustart),
- stellt den Hotspot auf den Kanal des Client-WLANs; ohne Client-WLAN auf 2,4 GHz Kanal 6 (5 GHz Kanal 36–64 ist in der EU nur drinnen erlaubt),
- pausiert den Hotspot höchstens alle 5 min für 20 s, wenn ein bekanntes WLAN in Reichweite ist, der Client sich aber nicht verbindet.

Der Hotspot vergibt Adressen per DHCP (NetworkManager, `ipv4.method shared`). Ist der Pi zusätzlich in deinem WLAN, leitet er den Internetzugang weiter, sonst hat das Handy im Hotspot kein Internet (nur der Kartenhintergrund im GUI fehlt dann, Leaflet liegt lokal im GUI). Weil Docker die FORWARD-Kette auf DROP setzt, trägt `surfcam-ap.sh` zwei Regeln in `DOCKER-USER` ein (nur Hotspot ↔ `wlan0`).

**Getestet (06.11.2026):** Hotspot auf Kanal 36 neben dem Heim-WLAN; Heim-WLAN getrennt und wieder verbunden, während der Hotspot lief: der Client verbindet sich, der Chip nimmt den Hotspot auf den neuen Kanal mit, beide bleiben aktiv.

**Einschränkungen:**

- Wechselt der Router den Kanal, ist der Hotspot bis zu 30 s weg.
- Ist der Router auf einem DFS-Kanal (5 GHz, 52–144), schaltet sich der Hotspot ab, solange der Pi dort verbunden ist (Hotspots sind dort nicht erlaubt). Abhilfe: im Router einen festen Kanal 36–48 einstellen.
- Beide Netze teilen sich Antenne und Sendezeit. Für das GUI reicht das. Robuster wäre ein USB-WLAN-Stick nur für den Hotspot.

## Installation

```
cd ~/SurfcamTracker && git pull
sudo pi/hotspot/install.sh <Name> <Passwort>     # Passwort mind. 8 Zeichen
```

Das Passwort steht nur in der NetworkManager-Verbindung `surfcam-hotspot` auf dem Pi (`/etc/NetworkManager/system-connections/`, nur root lesbar), nicht im Repo.

Ändern: `install.sh` erneut ausführen. Entfernen: `sudo pi/hotspot/uninstall.sh`.

**Sicher installieren** (falls etwas schiefgeht, verliert der Pi sonst sein WLAN): vorher einen automatischen Rückbau planen und ihn erst stoppen, wenn der Pi weiter erreichbar ist:

```
sudo systemd-run --on-active=300 --unit=surfcam-ap-revert /bin/bash ~/SurfcamTracker/pi/hotspot/uninstall.sh
sudo pi/hotspot/install.sh <Name> <Passwort>
# Pi noch erreichbar?
sudo systemctl stop surfcam-ap-revert.timer
```

Log: `journalctl -u surfcam-ap -f` · Status: `nmcli con show --active`, `/sbin/iw dev`

| Datei | Ziel |
|---|---|
| [surfcam-ap.sh](surfcam-ap.sh) | `/usr/local/sbin/surfcam-ap` |
| [surfcam-ap.service](surfcam-ap.service), [surfcam-ap.timer](surfcam-ap.timer) | `/etc/systemd/system/` |
| [install.sh](install.sh), [uninstall.sh](uninstall.sh) | Einrichten / Entfernen |
