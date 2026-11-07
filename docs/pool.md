# Surf-Pool-Modus

Ein iPhone filmt fest montiert und sendet einen Livestream an den Pi. Der Pi hält die letzten 150 s im Arbeitsspeicher. Auf Knopfdruck speichert er die letzten x Sekunden als MP4: im Test-GUI (Tab **Pool**) und später über die Taste am Tracker.

```
iPhone (Moblin)                     Pi
  4K HEVC, SRT ──WLAN/USB──► ffmpeg (wartet auf Port 8890/UDP, kopiert nur)
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
2. Mit dem Netz verbinden, in dem der Pi ist: WLAN des Pi-Hotspots, Heim-WLAN oder USB (siehe unten).
3. Im GUI (Tab **Pool**) die Ziel-Adresse ablesen, z. B. `srt://192.168.4.1:8890` im Hotspot oder `srt://192.168.8.125:8890` im Heim-WLAN.

**Stream anlegen:**

1. Moblin öffnen, Zahnrad (Einstellungen) → **Streams** → neuen Stream erstellen (Assistent). Bei der Plattform **Custom** wählen, Protokoll **SRT**, nicht SRTLA.
2. **URL:** die Adresse aus dem Tab Pool, z. B. `srt://192.168.4.1:8890`. Eine Stream-ID braucht der Pi nicht. Fragt der Assistent danach, das Feld leer lassen oder `pool` eintragen.
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
| «Stream stockt», Datenrate schwankt stark | WLAN zu langsam: Bitrate auf 20 oder 15 Mbit/s senken, oder per USB verbinden |
| Video im Tab Pool zeigt `h264` | Codec in Moblin steht noch auf H.264. Das geht auch, braucht aber etwa 50 Mbit/s für 4K |
| Clip deutlich kürzer als eingestellt | Stream wurde innerhalb der Clip-Länge neu verbunden. Ein Clip enthält nur Video seit dem letzten Verbindungsaufbau |
| iPhone wird heiss, Bildrate fällt | 4K-Kodierung über lange Zeit: Hülle weg, Schatten, 30 statt 60 fps |

Tipp: Das iPhone während des Streamens ans Ladegerät hängen. 4K-Streaming braucht viel Akku.

## Larix Broadcaster (Alternative)

Nur sinnvoll mit Premium, oder gratis mit H.264 und Neustart alle 30 min.

1. *Einstellungen → Verbindungen → neu:* URL `srt://<IP des Pi>:8890`, Modus **Caller**, Latenz 500 ms.
2. *Einstellungen → Video:* 3840×2160, 30 fps, Codec HEVC (Premium) oder H.264 (gratis, ca. 40–50 Mbit/s), Keyframe-Intervall 1 s.
3. Roter Knopf zum Starten.

## Verbindung iPhone – Pi

| Weg | Datenrate | Bemerkung |
|---|---|---|
| **USB-Kabel** (iPhone: Persönlicher Hotspot) | stabil, ohne Funk | iPhone «Persönlicher Hotspot» einschalten, Kabel an den Pi. Der Pi bekommt eine Adresse 172.20.10.x vom iPhone, die Ziel-Adresse für die App steht im Tab Pool (USB). Der Persönliche Hotspot muss im Mobilfunk-Abo erlaubt sein. Der Pi lädt das iPhone nur langsam (USB max. 0,6 A, mit 5-A-Netzteil 1,6 A). *Noch nicht getestet.* |
| **Hotspot des Pi** | 5 GHz: problemlos; 2,4 GHz: knapp | Ohne Heim-WLAN sendet der Hotspot auf 2,4 GHz Kanal 6. Für 4K dann die App auf 20 Mbit/s stellen. 5 GHz (Kanal 36–48) ist in der Schweiz und der EU nur **in Gebäuden** erlaubt. Im Freien wären es DFS-Kanäle, die der Hotspot des Pi nicht beherrscht. Siehe [Hotspot](../pi/hotspot/README.md). |
| **WLAN vor Ort** | je nach Netz | Pi und iPhone im selben WLAN, Gäste-WLANs trennen die Geräte oft voneinander. |

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
- Server-Option `--pool-port` (Standard 8890, `0` = aus).
- API: `GET /api/pool`, `POST /api/pool/clip {seconds}`, `POST /api/pool/settings {clip_seconds}`, `POST /api/pool/delete {name}`, `GET /api/pool/clips/<name>` (mit HTTP-Range, `?dl=1` zum Herunterladen).
