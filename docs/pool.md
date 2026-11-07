# Surf-Pool-Modus

Ein iPhone filmt fest montiert und sendet einen Livestream an den Pi. Der Pi hält die letzten 150 s im Arbeitsspeicher. Auf Knopfdruck speichert er die letzten x Sekunden als MP4: im Test-GUI (Tab **Pool**) und später über die Taste am Tracker.

```
iPhone (Moblin)                     Pi
  4K HEVC, SRT ──Hotspot──► ffmpeg (wartet auf Port 8890/UDP, kopiert nur)
                                 │
                                 ▼
                       1-s-Stücke im RAM (/dev/shm/surfcam-pool, 150 s)
                                 │  Knopf: Tab Pool oder (später) Tracker-Taste
                                 ▼
                       ~/surfcam-data/clips/JJJJMMTT_hhmmss_30s.mp4
```

- **Keine Neukodierung:** Der Pi kopiert das Video nur, das iPhone kodiert. Der Pi 5 hat keinen Hardware-Encoder. Ein 4K-Teststream, den der Pi selbst kodierte, lief mit 350 % CPU nicht in Echtzeit, das Kopieren braucht dagegen kaum Rechenzeit.
- **Genauigkeit:** Ein Stück beginnt immer an einem Schlüsselbild. In der App den Schlüsselbild-Abstand auf **1 s** stellen, dann ist ein Clip auf ±1 s genau. Bei 2 s sind es ±2 s.
- **Ende des Clips:** Gespeichert wird bis zum Knopfdruck. Der Pi wartet noch bis 4 s, bis das Stück mit dem Knopfdruck fertig ist. Das Speichern dauert danach etwa 1 s.
- **Unterbruch:** Bricht der Stream ab, wartet der Pi automatisch auf den nächsten. Ein Clip enthält nur Video seit dem letzten Verbindungsaufbau.
- **Speicher:** 4K HEVC mit 30 Mbit/s braucht etwa 3,75 MB/s, also 560 MB RAM für 150 s und 110 MB pro 30-s-Clip. Die Clips bleiben auf der SD-Karte, bis sie im GUI gelöscht werden.
- **Abspielen:** HEVC-Clips laufen auf iPhone, iPad und Mac direkt, unter Windows je nach Gerät nur mit der HEVC-Erweiterung. Wer Clips überall abspielen will, stellt die App auf H.264 und gibt mehr Bitrate (ca. 50 Mbit/s für 4K).

## Welche App?

Der Pi braucht einen **SRT-Stream** (MPEG-TS, H.264 oder HEVC). Stand November 2026:

| App | Kosten | Eignung |
|---|---|---|
| **Moblin** (Erik Moqvist, Open Source) | gratis, alle Funktionen frei. Kaufbar sind nur App-Symbole als Spende | **empfohlen:** SRT, HEVC, bis 4K/60 fps, Schlüsselbild-Abstand und B-Frames einstellbar, keine Zeitgrenze |
| Larix Broadcaster (Softvelum) | gratis mit Grenzen, Premium ca. 10 USD/Monat | **HEVC nur mit Premium.** Gratis stoppt der Stream nach 30 min plus 30 min mit eingeblendetem Hinweis. Gratis also nur H.264 und ständig neu starten |
| Blackmagic Camera | gratis | kann seit Version 3.2 an eigene SRT-Server senden. Codec und Bitrate für eigene Server konnte ich nicht klären, nicht getestet |
| Final Cut Camera (Apple) | gratis | **geht nicht:** Sendet nur an Final Cut Pro auf dem iPad (Live Multicam), kein SRT oder RTMP |

## Moblin einrichten (iPhone)

Moblin aus dem App Store laden (iOS 16.4 oder neuer). Die Menünamen stammen aus der Projektbeschreibung und können in der App leicht anders heissen. Massgebend sind die Werte.

**Vorher am iPhone:**

1. *Einstellungen → Anzeige & Helligkeit → Automatische Sperre → Nie*. Sonst geht das Display aus, und iOS hält die Kamera an (Moblin streamt im Hintergrund nur Ton mit Standbild).
2. iPhone immer mit dem **WLAN-Hotspot des Pi** verbinden (siehe [Hotspot](../pi/hotspot/README.md)). Der Pi hat dort immer die Adresse `192.168.4.1`, zuhause wie im Pool. Moblin muss deshalb nur einmal eingerichtet werden. Hat der Hotspot kein Internet, meldet iOS das. Das iPhone bleibt trotzdem verbunden.
3. Die Ziel-Adresse ist immer `srt://192.168.4.1:8890` (steht auch im Tab **Pool**).

**Stream anlegen:**

1. Moblin öffnen, Zahnrad (Einstellungen) → **Streams** → neuen Stream erstellen (Assistent). Bei der Plattform **Custom** wählen, Protokoll **SRT**, nicht SRTLA.
2. **URL:** `srt://192.168.4.1:8890`. Eine Stream-ID braucht der Pi nicht. Fragt der Assistent danach, das Feld leer lassen oder `pool` eintragen.
3. Stream speichern und auswählen (Häkchen), dann den Stream öffnen und einstellen:

| Bereich | Einstellung | Wert | Warum |
|---|---|---|---|
| Video | Auflösung | **2160p (4K)** | |
| Video | Bildrate | **30 fps**, bei schnellen Bewegungen 60 fps | 60 fps braucht etwa die doppelte Bitrate |
| Video | Codec | **H.265/HEVC** | halbe Datenmenge gegenüber H.264 |
| Video | Bitrate | **30 Mbit/s**; über den 2,4-GHz-Hotspot 20 Mbit/s | |
| Video | Max. Schlüsselbild-Abstand | **1 s** | Clips sind dann auf ±1 s genau |
| Video | B-Frames | **aus** | weniger Verzögerung |
| Video | Adaptive Bitrate | **aus** | gleichbleibende Qualität. Nur einschalten, wenn das WLAN schwach ist |
| SRT | Latenz | **500 ms** | Der Pi rechnet mit etwa 1 s Verzögerung bis zum Bild. Bei mehr Latenz kann der Moment des Knopfdrucks am Clip-Ende fehlen |
| Audio | Bitrate | 128 kbit/s | Ton wird im Clip mitgespeichert |

4. Zurück ins Kamerabild, das iPhone **quer** halten und auf **Go Live** tippen.
5. Im GUI, Tab Pool, steht jetzt «empfängt» mit `hevc 3840x2160 30 fps`, die Datenrate und der wachsende Puffer (max. 150 s).
6. Test: «Letzte 30 s speichern» drücken, dann den Clip unter **Clips** mit «Ansehen» abspielen.

**Wenn es nicht geht:**

| Anzeige | Ursache |
|---|---|
| Moblin verbindet nicht, Tab Pool bleibt bei «wartet auf Stream» | falsche IP, oder iPhone und Pi sind nicht im selben Netz. Protokoll SRT statt SRTLA? |
| «Stream stockt», Datenrate schwankt stark | WLAN zu langsam: Bitrate auf 20 oder 15 Mbit/s senken. Hotspot auf 2,4 GHz (Heim-WLAN auf 2,4 GHz)? Siehe unten |
| Video im Tab Pool zeigt `h264` | Codec in Moblin steht noch auf H.264. Das geht auch, braucht aber etwa 50 Mbit/s für 4K |
| Clip deutlich kürzer als eingestellt | Stream wurde innerhalb der Clip-Länge neu verbunden. Ein Clip enthält nur Video seit dem letzten Verbindungsaufbau |
| iPhone wird heiss, Bildrate fällt | 4K-Kodierung über lange Zeit: Hülle weg, Schatten, 30 statt 60 fps |

Tipp: Das iPhone während des Streamens ans Ladegerät hängen. 4K-Streaming braucht viel Akku.

## Larix Broadcaster (Alternative)

Nur sinnvoll mit Premium, oder gratis mit H.264 und Neustart alle 30 min.

1. *Einstellungen → Verbindungen → neu:* URL `srt://<IP des Pi>:8890`, Modus **Caller**, Latenz 500 ms.
2. *Einstellungen → Video:* 3840×2160, 30 fps, Codec HEVC (Premium) oder H.264 (gratis, ca. 40–50 Mbit/s), Keyframe-Intervall 1 s.
3. Roter Knopf zum Starten.

## iPhone fernsteuern (Moblin Remote control)

Im Tab Pool, Karte **iPhone fernsteuern**, steuert der Pi die App Moblin. In Moblin heisst das «Remote control». Der Pi spielt dabei das Gerät «Assistant», das iPhone ist der «Streamer».

| Möglich | Nicht möglich (Moblin bietet es nicht an) |
|---|---|
| Zoom stufenlos und Zoom-Stufen (z. B. 0,5× Weitwinkel, 1×, 3×), Stream starten/stoppen, Aufnahme auf dem iPhone, stumm, Licht, Stealth-Modus. Anzeige von Akku, «lädt», Temperatur (normal/warm/heiss) und Kamera. Vorschaubild etwa 1 pro Sekunde | Fokus, Belichtung, ISO, Weissabgleich (bleiben auf Automatik), Auflösung und Bildrate (in Moblin selbst einstellen) |

**Einmal einrichten:**

1. Im GUI, Tab Pool, Karte «iPhone fernsteuern», **Verbinden** drücken. Der Pi wartet jetzt auf TCP-Port 2345.
2. «Einrichten in Moblin» aufklappen. Dort stehen das **Passwort** und die **Assistant-URL** `ws://192.168.4.1:2345`.
3. Entweder in Moblin von Hand: *Einstellungen → Remote control*:
   - Passwort eintragen.
   - Unter *Streamer* «Enabled» einschalten und die Assistant-URL eintragen.
4. Oder schneller: das GUI in Safari **auf dem Kamera-iPhone** öffnen und den Link «Moblin einrichten (…)» antippen. Moblin fragt nach und übernimmt Passwort und URL. Das geht nicht, während Moblin streamt oder aufnimmt.

**Im Betrieb:**
- «Verbinden» schaltet den Empfang am Pi ein. Moblin versucht laufend, sich zu verbinden, höchstens alle 10 s, und meldet sich deshalb nach spätestens etwa 10 s.
- «Trennen» schaltet den Empfang ab. Die Einstellung bleibt nach einem Neustart des Pi erhalten.
- Nach einem **falschen Passwort** versucht Moblin es nicht mehr. Dann in Moblin das Passwort korrigieren oder Remote control aus- und wieder einschalten.
- Ein **gestoppter Stream** füllt den Puffer nicht mehr. Deshalb fragt das GUI vor dem Stoppen nach.
- **Stealth-Modus:** Moblin macht das Display schwarz und streamt weiter. Das spart Akku und hält das iPhone kühler.

Sicherheit: Das Passwort liegt nur auf dem Pi (`~/surfcam-data/moblin.json`, Rechte 600), nicht im Repo. Das GUI hat keine Anmeldung. Wer es öffnen kann, sieht auch das Passwort.

Mit dem echten Moblin über den Hotspot getestet (07.11.2026): Anmeldung über den Einrichtungs-Link, Zoom-Stufen 0,5×/1×/2×, Akku, Temperatur, Kamera und Vorschaubild (JPEG, ca. 6 kB) kommen an. Davor mit einem nachgebauten Client: falsches Passwort, Ping, Zoom, Stealth-Modus, Licht. Grundlage ist der Quellcode von Moblin (`Moblin/RemoteControl/RemoteControl.swift`, API-Version 0.1).

## Verbindung iPhone – Pi

Der Stream läuft **immer über den WLAN-Hotspot des Pi** (`192.168.4.1`). Das GUI zeigt nur diese Adressen an.

| Lage | Hotspot-Kanal | Bitrate in Moblin |
|---|---|---|
| Pi zusätzlich im Heim-WLAN (z. B. zuhause, Kanal 36) | gleicher Kanal wie das Heim-WLAN (5 GHz, bei einem 2,4-GHz-Heim-WLAN aber 2,4 GHz: dann 20 Mbit/s) | 30 Mbit/s |
| Ohne Heim-WLAN (z. B. im Pool) | 5 GHz Kanal 36 | 30 Mbit/s |
| Heim-WLAN auf einem DFS-Kanal (52–144) | Hotspot aus | geht nicht. Pi aus dem Heim-WLAN nehmen oder den Router auf Kanal 36–48 stellen |

Hinweis: Die Kanäle 36–48 sind in der Schweiz und der EU nur **in Gebäuden** erlaubt. Im Freien wären es DFS-Kanäle, die der Hotspot des Pi nicht beherrscht. Für den Betrieb im Freien den Rückfallkanal auf 2,4 GHz zurückstellen (`FALLBACK_CH=6` in [surfcam-ap.sh](../pi/hotspot/surfcam-ap.sh), dann `sudo install -m 755 pi/hotspot/surfcam-ap.sh /usr/local/sbin/surfcam-ap`) und in Moblin 20 Mbit/s wählen.

Weitere Wege, die der Pi auch annimmt, aber nicht im GUI angezeigt werden: USB-Kabel mit dem «Persönlichen Hotspot» des iPhones (Adresse 172.20.10.x, nicht getestet) und ein WLAN vor Ort.

## Test ohne iPhone

Auf dem Pi einen Teststream senden (vorher kodieren, der Pi schafft das nicht in Echtzeit):

```
ffmpeg -f lavfi -i testsrc2=size=1920x1080:rate=30 -t 75 -c:v libx265 -preset ultrafast \
  -x265-params keyint=30 -b:v 10M -f mpegts /tmp/test.ts
ffmpeg -re -i /tmp/test.ts -c copy -f mpegts 'srt://127.0.0.1:8890?pkt_size=1316'
```

## Technik

- [pi/testgui/pool.py](../pi/testgui/pool.py): ffmpeg als SRT-Listener (`-f segment -segment_time 1`, MPEG-TS). Clips mit dem `concat:`-Protokoll von ffmpeg nach MP4 (`-c copy -movflags +faststart`, HEVC mit `hvc1`-Tag für Apple).
- Die Endzeit eines Stücks ist die Änderungszeit seiner Datei, also der Zeitpunkt des Empfangs. Das Bild kommt etwa 1 s nach der Aufnahme an (Kodierung im iPhone, SRT-Latenz 0,5 s). Der Clip reicht deshalb bis 1 s nach dem Knopfdruck (`STREAM_LAG_S` in `pool.py`), damit der Moment des Knopfdrucks sicher drin ist.
- Server-Optionen `--pool-port` (Standard 8890, `0` = aus) und `--moblin-port` (Fernsteuerung, Standard 2345, `0` = aus).
- [pi/testgui/moblin.py](../pi/testgui/moblin.py): kleiner WebSocket-Server (nur Standardbibliothek) und das Remote-control-Protokoll von Moblin.
- API: `GET /api/pool`, `POST /api/pool/clip {seconds}`, `POST /api/pool/settings {clip_seconds}`, `POST /api/pool/delete {name}`, `GET /api/pool/clips/<name>` (mit HTTP-Range, `?dl=1` zum Herunterladen), `POST /api/pool/phone {action: connect|disconnect|zoom|zoom_preset|live|record|mute|torch|stealth, value}`, `GET /api/pool/phone/preview`.
