# DGPS-Test: Tracker mit dem Pi-GPS korrigieren

Der Pi hat auf dem Gateway-HAT ein eigenes GNSS-Modul (gleiche Chipfamilie wie der L76K im Tracker, siehe [docs/gateway.md](../../docs/gateway.md)). Beide Empfänger machen zur selben Zeit ähnliche Fehler: Atmosphäre, Satellitenbahnen und -uhren sind für beide gleich. Zieht man die Pi-Position von der Tracker-Position zur selben Zeit ab, fallen diese gemeinsamen Fehler heraus.

Das ist **Differenz-GPS im Positionsbereich**. Beide Module liefern keine Rohdaten, echtes RTK (cm) ist damit nicht möglich. Wie viel die Korrektur bringt, hängt davon ab, ob beide Empfänger dieselben Satelliten verwenden. Am Strand mit freier Sicht eher ja, auf dem Wasser (Körper, Wellen) weniger. Deshalb: messen.

```
roh        = Tracker(t) − Kamerastandort
korrigiert = Tracker(t) − Pi(t) − Versatz
```

- **Kamerastandort:** Mittel der Tracker-Positionen, während der Tracker neben dem Pi liegt.
- **Versatz:** Mittel von Tracker − Pi im selben Zeitraum. Er enthält den Abstand der Antennen und die feste Abweichung zwischen den beiden Empfängern.
- **Gleiche Zeit:** verglichen wird über die UTC-Zeit des Fixes. Das Pi-GPS läuft mit 1 Hz (mit 10 Hz hing das HAT-Modul), dazwischen wird interpoliert. Der gemeinsame Fehler ändert sich über Minuten, das reicht.

Für die Kamera zählt nur die Lage des Trackers **relativ zur Kamera**. Ein gemeinsamer Fehler beider Empfänger stört dabei nicht, ein unterschiedlicher schon.

## Nullbasis-Test (zuhause möglich)

Tracker direkt neben die Antenne des Pi legen, beide mit freier Sicht zum Himmel (Fenster, Balkon). Der wahre Abstand ist dann null, jeder gemessene Abstand ist Fehler.

1. Test-GUI, Tab **Position**: warten, bis Pi-GPS und Tracker einen Fix haben.
2. **Kamerastandort festlegen** (60 s).
3. 30–60 min liegen lassen. Die Tabelle «Vergleich roh / korrigiert» und das Streudiagramm zeigen live, was die Korrektur bringt.
4. Für die Auswertung am Rechner: im Tab Position die Aufzeichnung «GPS des Pi» herunterladen, dazu das Tracker-Log aus dem Flash (Tab Log, 5 Hz) oder «Empfangene Tracker-Pakete» (3,3 bzw. 10 Pakete/s).

```
python3 dgps_eval.py --pi pi_gnss_20261107_090000.csv --rover 261107_090512.csv --calib 60
```

| Option | |
|---|---|
| `--pi` | eine oder mehrere Aufzeichnungen des Pi-GPS |
| `--rover` | Tracker: Flash-Log (CSV aus dem Tab Log) oder Paket-Aufzeichnung (`rx_….csv`) |
| `--calib 60` | Kamerastandort und Versatz aus den ersten 60 s, wie im GUI. Der Verlauf zeigt dann, wie stark roh und korrigiert über die Zeit wegdriften |
| `--calib 0` | (Standard) über alle Daten: misst nur die Streuung |
| `--block 300` | Verlauf in Blöcken à 300 s |
| `--out werte.csv` | jedes Wertepaar als CSV (zum Plotten) |

Ausgabe: Anzahl gemeinsamer Zeitpunkte, Versatz, Anteil gleicher Satellitenzahl, Abstand von der Mitte (Mittel, 95 %, Maximum, Streuung) für roh und korrigiert, Verlauf.

**Beurteilung:** Entscheidend ist der 95-%-Wert über längere Zeit (≥ 30 min) mit `--calib 60`. Ist «korrigiert» klar kleiner als «roh», lohnt sich die Korrektur im SurfCam-Dienst. Bringt sie wenig, reicht der Kamerastandort aus dem Tracker allein.

## Aufzeichnungen auf dem Pi

Das Test-GUI schreibt nach `~/surfcam-data/` (eine Datei pro Start und UTC-Tag, nach 30 Tagen gelöscht):

| Ordner | Inhalt |
|---|---|
| `pi_gnss/` | jede Position des Pi-GPS: `date,utc,time_ms,lat,lon,alt,fix,sats,hdop` (1 Hz, ≈ 6 MB/Tag) |
| `rx/` | jedes empfangene Tracker-Paket: `rx_utc` + Felder des Pakets |
| `position.json` | Mittelwert des Pi-GPS und Kamerastandort (bleiben über Neustarts erhalten) |

Synthetisch geprüft (gemeinsamer Fehler bis ≈ 2 m, eigenes Rauschen 0,3 m pro Empfänger): 95 % roh 3,4 m, korrigiert 1,0 m. Mit echten Daten steht der Test noch aus.
