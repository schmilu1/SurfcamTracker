# Reichweitentest

[rangetest.py](rangetest.py) läuft auf dem Pi und wertet die Rover-Pakete aus, die das Gateway über MQTT bereitstellt. Es braucht nur Python 3 (Standardbibliothek) und den laufenden Container `surfcam-mosquitto-1`.

Alle 10 s erscheint eine Zeile:

```
14:02:10 , 98 Pakete, Verlust   2.0 %, 9.8/s, RSSI Ø  -88 min  -97 dBm, SNR Ø   7.5 min   1.2 dB, Distanz  480 m, Fix 98/98
```

Jedes Paket landet zusätzlich in `rangetest_JJJJMMTT_hhmmss.csv` (Empfangszeit, Band, Sequenz, verlorene Pakete davor, RSSI, SNR, Position, Entfernung …).

## Ablauf am Strand

Vorbereitung (einmal): Repo auf dem Pi holen.

```
cd ~ && git clone https://github.com/schmilu1/SurfcamTracker.git
```

Pro Band:

1. Gateway umschalten: `~/sx1302_hal_rpi5-master/surfcam/band.sh A` (bzw. `B`)
2. Rover umschalten (USB): `AT+BAND=A` (bzw. `B`), mit `ATI` prüfen: `Band A … Sendeabstand 300 ms` bzw. `Band B … 100 ms`
3. Auswertung starten:
   ```
   cd ~/SurfcamTracker/tools/rangetest
   python3 rangetest.py --ref <lat>,<lon>     # Standort des Gateways bzw. der Kamera
   ```
4. Mit dem Rover ins Wasser, gleiche Strecke wie beim anderen Band (z. B. 10 min Line-up, Paddeln, einige Wellen).
5. Strg+C: Gesamtwerte und Name der CSV erscheinen.

Ohne `--ref` fehlt nur die Entfernung.

## Worauf achten

| Wert | Bedeutung |
|---|---|
| Verlust | wichtigster Wert; entscheidend ist, wie viele Positionen pro Sekunde ankommen (`/s`), nicht der Prozentsatz allein |
| RSSI min | unter ≈ −115 dBm wird es knapp (Empfindlichkeit SF7/250 kHz ≈ −120 dBm) |
| SNR min | unter ≈ −5 dB wird es knapp (Grenze SF7 ≈ −7,5 dB) |

Band B gewinnt, wenn dort trotz höherem Verlust mehr Positionen pro Sekunde ankommen und die Ausfälle nicht länger dauern als bei Band A. Die längsten Lücken zeigt die CSV (`lost_before`).

Eine Zeile erscheint nur, wenn Pakete ankommen. Bleibt die Ausgabe stehen, ist die Verbindung ganz weg.
