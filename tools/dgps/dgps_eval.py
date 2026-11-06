#!/usr/bin/env python3
"""Differenz-GPS im Positionsbereich auswerten: Tracker roh gegen korrigiert mit dem Pi-GPS.

  python3 dgps_eval.py --pi pi_gnss_20261107_090000.csv --rover tracker_log.csv
  python3 dgps_eval.py --pi pi1.csv pi2.csv --rover rx_20261107_090000.csv --calib 60 --block 300

Eingaben (aus dem Test-GUI):
  --pi     Aufzeichnung des Pi-GPS (Tab Position, "GPS des Pi"), 10 Hz
  --rover  Tracker: Log aus dem Flash (Tab Log, CSV, 5 Hz) oder empfangene Pakete (Tab Position, "rx")

Für jede Tracker-Position wird die Pi-Position mit derselben UTC-Zeit (auf 100 ms) gesucht.
  roh        = Tracker - Kamerastandort       (Mittel der Tracker-Positionen im Kalibrierzeitraum)
  korrigiert = Tracker - Pi - Versatz         (Versatz = Mittel von Tracker - Pi im Kalibrierzeitraum)
Liegt der Tracker die ganze Zeit neben dem Pi (Nullbasis-Test), ist jeder Abstand von 0 Fehler.

--calib 0 (Standard): Kalibrierung über alle Daten, misst nur die Streuung.
--calib 60: die ersten 60 s, wie im GUI. Zeigt zusätzlich, wie stark die Fehler danach wegdriften.
"""

import argparse
import bisect
import csv
import math
import sys
from datetime import datetime, timezone

R_EARTH = 6371000.0


def enu(lat, lon, lat0, lon0):
    return (math.radians(lon - lon0) * R_EARTH * math.cos(math.radians(lat0)),
            math.radians(lat - lat0) * R_EARTH)


def day_ms(date_iso):
    return int(datetime.fromisoformat(date_iso).replace(tzinfo=timezone.utc).timestamp() * 1000)


def key(ms):
    return int(round(ms / 100.0)) * 100


def read_pi(paths):
    out = {}
    for p in paths:
        with open(p, newline="") as f:
            for r in csv.DictReader(f):
                if r.get("fix") in ("", "0") or not r.get("lat"):
                    continue
                out[key(day_ms(r["date"]) + int(r["time_ms"]))] = (float(r["lat"]), float(r["lon"]), int(r["sats"]))
    return out


def pi_at(pi, keys, t):
    """Pi-Position zur Zeit t: genau oder zwischen zwei Positionen (Abstand ≤ 1,1 s) interpoliert."""
    if t in pi:
        return pi[t]
    i = bisect.bisect_left(keys, t)
    if 0 < i < len(keys) and keys[i] - keys[i - 1] <= 1100:
        a, b = pi[keys[i - 1]], pi[keys[i]]
        f = (t - keys[i - 1]) / (keys[i] - keys[i - 1])
        return (a[0] + (b[0] - a[0]) * f, a[1] + (b[1] - a[1]) * f, a[2])
    return None


def read_rover(path):
    """-> Liste (abs_ms, lat, lon, sats). Flash-Log (date,utc,time_ms,...) oder rx (rx_utc,...,utc_ms,...)."""
    out = []
    with open(path, newline="") as f:
        rows = csv.DictReader(f)
        fields = rows.fieldnames or []
        for r in rows:
            if r.get("fix") in ("", "0", None) or not r.get("lat"):
                continue
            if "time_ms" in fields:      # Flash-Log: time_ms läuft über Mitternacht hinaus weiter
                t = day_ms(r["date"]) + int(r["time_ms"])
            elif "rx_utc" in fields:     # empfangene Pakete
                rx = datetime.fromisoformat(r["rx_utc"])
                if not r.get("utc_ms"):
                    continue
                t = day_ms(rx.date().isoformat()) + int(r["utc_ms"])
                if t - rx.timestamp() * 1000 > 43200000:
                    t -= 86400000
                elif rx.timestamp() * 1000 - t > 43200000:
                    t += 86400000
            else:
                sys.exit("%s: unbekanntes Format (erwartet Flash-Log oder rx-Aufzeichnung)" % path)
            out.append((key(t), float(r["lat"]), float(r["lon"]), int(r.get("sats") or 0)))
    out.sort()
    return out


def stats(vs):
    if not vs:
        return None
    d = sorted(math.hypot(e, n) for e, n in vs)
    me = sum(e for e, _ in vs) / len(vs)
    mn = sum(n for _, n in vs) / len(vs)
    spread = math.sqrt(sum((e - me) ** 2 + (n - mn) ** 2 for e, n in vs) / len(vs))
    return {"n": len(vs), "mean": sum(d) / len(d), "p95": d[int(0.95 * (len(d) - 1))], "max": d[-1],
            "spread": spread}


def fmt(s):
    if not s:
        return "%6s %7s %7s %7s %8s" % ("–", "–", "–", "–", "–")
    return "%6d %6.2fm %6.2fm %6.2fm %7.2fm" % (s["n"], s["mean"], s["p95"], s["max"], s["spread"])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--pi", nargs="+", required=True, help="CSV des Pi-GPS")
    ap.add_argument("--rover", required=True, help="CSV des Trackers (Flash-Log oder rx)")
    ap.add_argument("--calib", type=float, default=0, help="Kalibrierzeitraum in s ab Beginn (0 = alle Daten)")
    ap.add_argument("--block", type=float, default=300, help="Blocklänge für den Zeitverlauf in s (Standard 300)")
    ap.add_argument("--out", help="CSV mit jedem Wertepaar schreiben")
    args = ap.parse_args()

    pi = read_pi(args.pi)
    rover = read_rover(args.rover)
    keys = sorted(pi)
    pairs = [(t, la, lo, s, q) for t, la, lo, s in rover for q in [pi_at(pi, keys, t)] if q]
    exact = sum(1 for p in pairs if p[0] in pi)
    print("Tracker-Positionen mit Fix: %d, davon mit Pi-Position: %d (%d zeitgleich, %d interpoliert)" % (
        len(rover), len(pairs), exact, len(pairs) - exact))
    if len(pairs) < 10:
        sys.exit("zu wenige gemeinsame Zeitpunkte: überlappen die Aufzeichnungen zeitlich?")
    t0 = pairs[0][0]
    cal = [p for p in pairs if not args.calib or p[0] - t0 <= args.calib * 1000]
    lat0 = sum(p[4][0] for p in pairs) / len(pairs)
    lon0 = sum(p[4][1] for p in pairs) / len(pairs)
    cam_lat = sum(p[1] for p in cal) / len(cal)
    cam_lon = sum(p[2] for p in cal) / len(cal)
    d = [enu(p[1], p[2], p[4][0], p[4][1]) for p in cal]
    bias = (sum(x[0] for x in d) / len(d), sum(x[1] for x in d) / len(d))

    rows = []
    for t, la, lo, s, (pla, plo, ps) in pairs:
        raw = enu(la, lo, cam_lat, cam_lon)
        de, dn = enu(la, lo, pla, plo)
        corr = (de - bias[0], dn - bias[1])
        rows.append((t, raw, corr, s, ps))

    dur = (pairs[-1][0] - t0) / 1000
    print("Zeitraum: %s bis %s UTC (%.0f min)" % (
        datetime.fromtimestamp(t0 / 1000, timezone.utc).strftime("%Y-%m-%d %H:%M:%S"),
        datetime.fromtimestamp(pairs[-1][0] / 1000, timezone.utc).strftime("%H:%M:%S"), dur / 60))
    print("Kalibrierung: %s, %d Paare, Versatz Tracker - Pi %.2f m (Ost %.2f, Nord %.2f)" % (
        "alle Daten" if not args.calib else "erste %.0f s" % args.calib, len(cal), math.hypot(*bias), *bias))
    print("Pi-Mittel %.7f, %.7f · Kamerastandort aus Tracker %.2f m daneben" % (
        lat0, lon0, math.hypot(*enu(cam_lat, cam_lon, lat0, lon0))))
    same_sats = sum(1 for r in rows if r[3] == r[4]) / len(rows)
    print("gleiche Satellitenzahl bei Tracker und Pi: %.0f %%" % (same_sats * 100))
    print()
    print("Abstand von der Mitte   %6s %7s %7s %7s %8s" % ("n", "Mittel", "95 %", "max", "Streuung"))
    print("roh                     " + fmt(stats([r[1] for r in rows])))
    print("korrigiert              " + fmt(stats([r[2] for r in rows])))
    if args.block and dur > args.block * 1.5:
        print()
        print("Verlauf (Blöcke à %.0f s)  95 %% roh   95 %% korr." % args.block)
        b0 = t0
        while b0 <= pairs[-1][0]:
            blk = [r for r in rows if b0 <= r[0] < b0 + args.block * 1000]
            if blk:
                sr, sc = stats([r[1] for r in blk]), stats([r[2] for r in blk])
                print("  %s  %5d   %6.2f m   %6.2f m" % (
                    datetime.fromtimestamp(b0 / 1000, timezone.utc).strftime("%H:%M:%S"), len(blk), sr["p95"], sc["p95"]))
            b0 += args.block * 1000
    if args.out:
        with open(args.out, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["utc", "raw_e", "raw_n", "corr_e", "corr_n", "sats_tracker", "sats_pi"])
            for t, raw, corr, s, ps in rows:
                w.writerow([datetime.fromtimestamp(t / 1000, timezone.utc).isoformat(timespec="milliseconds"),
                            "%.3f" % raw[0], "%.3f" % raw[1], "%.3f" % corr[0], "%.3f" % corr[1], s, ps])
        print("\nWerte geschrieben:", args.out)


if __name__ == "__main__":
    main()
