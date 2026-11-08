# Stromverbrauch des Pi

Der Tab **Strom** im Test-GUI zeigt alle 2 s, wie viel Leistung der Raspberry Pi 5 braucht. Er hält eine Stunde Verlauf und rechnet die Laufzeit mit einer Powerbank aus.

## Was gemessen wird

Der Pi 5 hat einen Power-Management-Chip (PMIC). Er misst Strom und Spannung seiner eigenen Spannungswandler, abgefragt mit `vcgencmd pmic_read_adc`.

| Gruppe | Wandler | Was daran hängt |
|---|---|---|
| CPU | `VDD_CORE` | die vier CPU-Kerne |
| RAM | `1V1_SYS`, `DDR_VDD2`, `DDR_VDDQ` | Arbeitsspeicher |
| WLAN | `3V7_WL_SW` | WLAN-/Bluetooth-Chip (Hotspot) |
| I/O | `0V8_SW`, `1V8_SYS`, `3V3_SYS`, `HDMI` usw. | RP1 (USB, GPIO), 3,3 V für HATs |

**Nicht gemessen**, sondern geschätzt:

| Anteil | Annahme | in `power.py` |
|---|---|---|
| Verluste der Wandler | Wirkungsgrad 85 % | `PMIC_EFFICIENCY` |
| LoRa-HAT (SX1302 + GNSS) an 5 V | 0,4 W | `EXTRA_HAT_W` |
| Lüfter | 0,8 W bei voller Drehzahl, linear mit PWM | `FAN_MAX_W` |
| USB-Geräte am Pi | nicht berücksichtigt | |

«Gesamt (geschätzt)» ist also eine Schätzung für den Verbrauch am Netzteil. Für genaue Werte braucht es ein USB-C-Messgerät zwischen Netzteil und Pi. Damit lassen sich auch die Annahmen oben anpassen.

## Anzeigen

- **Gesamt, Pi gemessen, Mittel 5 min, Spitze 1 h:** Leistung in Watt.
- **CPU, Temperatur, Lüfter:** Last über alle vier Kerne, Takt und Lüfterdrehzahl.
- **Energie:** aufsummiert seit dem Start des GUI.
- **Laufzeit mit Powerbank:** Kapazität in mAh eingeben, wie sie auf der Powerbank steht. Rechnung: mAh × 3,7 V × 85 % nutzbar ÷ Leistung.
- **Aufteilung:** alle Wandler mit Spannung, Strom und Leistung.
- **Prozesse:** die Programme mit der meisten CPU-Zeit in den letzten 2 s. 100 % entspricht einem vollen Kern. Im Pool-Modus sollte `ffmpeg` dort erscheinen.
- **Warnung Unterspannung / Drosselung:** aus `vcgencmd get_throttled`.
  - «Jetzt» bedeutet, dass es gerade passiert.
  - «Seit dem Start aufgetreten» bedeutet, dass es irgendwann seit dem Booten passiert ist.
  - Unterspannung heisst: Netzteil oder Kabel liefern zu wenig. Für den Pi 5 sind 5 V / 5 A (27 W) empfohlen. Bei einer Powerbank ohne 5-A-Modus kann das USB-Gerät am Pi zu wenig Strom bekommen.

## Messwerte

| Zustand | Pi gemessen | Gesamt geschätzt | CPU |
|---|---|---|---|
| Leerlauf (Gateway, GUI, kein Stream), 08.11.2026 | 2,1 W | ca. 3,1 W | < 2 % |

## API

`GET /api/power?since=<unix-zeit>` liefert die aktuelle Messung (`now`) und den Verlauf seit `since` (`history`: Zeit, Pi W, Gesamt W, °C, CPU %).
