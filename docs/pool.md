# Surf-Pool-Modus

Ein iPhone filmt fest montiert und sendet einen Livestream an den Pi. Der Pi hält die letzten 150 s im Arbeitsspeicher. Auf Knopfdruck speichert er die letzten x Sekunden als MP4: im Test-GUI (Tab **Pool**) und später über die Taste am Tracker.

```
iPhone (Larix Broadcaster)          Pi
  4K HEVC, SRT ──WLAN/USB──► ffmpeg (wartet auf Port 8890/UDP, kopiert nur)
                                 │
                                 ▼
                       1-s-Stücke im RAM (/dev/shm/surfcam-pool, 150 s)
                                 │  Knopf: Tab Pool oder (später) Tracker-Taste
                                 ▼
                       ~/surfcam-data/clips/JJJJMMTT_hhmmss_30s.mp4
```

- **Keine Neukodierung:** Der Pi kopiert das Video nur, das iPhone kodiert. Der Pi 5 hat keinen Hardware-Encoder. Ein 4K-Teststream, den der Pi selbst kodierte, lief mit 350 % CPU nicht in Echtzeit, das Kopieren braucht dagegen kaum Rechenzeit.
- **Genauigkeit:** Ein Stück beginnt immer an einem Schlüsselbild. In Larix den Schlüsselbild-Abstand auf **1 s** stellen, dann ist ein Clip auf ±1 s genau. Bei 2 s sind es ±2 s.
- **Ende des Clips:** Gespeichert wird bis zum Knopfdruck. Der Pi wartet noch bis 4 s, bis das Stück mit dem Knopfdruck fertig ist. Das Speichern dauert danach etwa 1 s.
- **Unterbruch:** Bricht der Stream ab, wartet der Pi automatisch auf den nächsten. Ein Clip enthält nur Video seit dem letzten Verbindungsaufbau.
- **Speicher:** 4K HEVC mit 30 Mbit/s braucht etwa 3,75 MB/s, also 560 MB RAM für 150 s und 110 MB pro 30-s-Clip. Die Clips bleiben auf der SD-Karte, bis sie im GUI gelöscht werden.
- **Abspielen:** HEVC-Clips laufen auf iPhone, iPad und Mac direkt, unter Windows je nach Gerät nur mit der HEVC-Erweiterung. Wer Clips überall abspielen will, stellt Larix auf H.264 und gibt mehr Bitrate (ca. 50 Mbit/s für 4K).

## Larix Broadcaster einrichten (iPhone)

App «Larix Broadcaster» (Softvelum, gratis) aus dem App Store.

1. **Einstellungen → Verbindungen → Neue Verbindung**
   - Name: `SurfCam`
   - URL: `srt://<IP des Pi>:8890`. Die IP steht im Tab Pool, z. B. `srt://192.168.4.1:8890` im Hotspot.
   - Modus: **Caller**, Latenz 500 ms, sonst nichts.
2. **Einstellungen → Video**
   - Auflösung 3840×2160, 30 fps
   - Codec **HEVC**
   - Bitrate 30 Mbit/s (bei Aussetzern 20 Mbit/s)
   - **Keyframe-Intervall 1 s**
3. **Einstellungen → Audio:** nach Wunsch. Ton wird mitgespeichert.
4. Im Hauptbild auf den roten Knopf tippen. Im Tab Pool steht dann «empfängt» mit Auflösung, Datenrate und Länge des Puffers.

Tipp: In Larix den Bildschirm-Timeout ausschalten, und das iPhone ans Ladegerät hängen. 4K-Streaming braucht viel Akku.

## Verbindung iPhone – Pi

| Weg | Datenrate | Bemerkung |
|---|---|---|
| **USB-Kabel** (iPhone: Persönlicher Hotspot) | stabil, ohne Funk | iPhone «Persönlicher Hotspot» einschalten, Kabel an den Pi. Der Pi bekommt eine Adresse 172.20.10.x vom iPhone, die Ziel-IP für Larix steht im Tab Pool (USB). Der Persönliche Hotspot muss im Mobilfunk-Abo erlaubt sein. Der Pi lädt das iPhone nur langsam (USB max. 0,6 A, mit 5-A-Netzteil 1,6 A). *Noch nicht getestet.* |
| **Hotspot des Pi** | 5 GHz: problemlos; 2,4 GHz: knapp | Ohne Heim-WLAN sendet der Hotspot auf 2,4 GHz Kanal 6. Für 4K dann Larix auf 20 Mbit/s stellen. 5 GHz (Kanal 36–48) ist in der Schweiz und der EU nur **in Gebäuden** erlaubt. Im Freien wären es DFS-Kanäle, die der Hotspot des Pi nicht beherrscht. Siehe [Hotspot](../pi/hotspot/README.md). |
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
