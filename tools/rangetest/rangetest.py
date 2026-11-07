#!/usr/bin/env python3
"""Reichweitentest SurfCam: Rover-Pakete vom Gateway (MQTT) auswerten.

Läuft auf dem Pi, nur Python-Standardbibliothek. Liest die Uplinks der ChirpStack
Gateway Bridge über mosquitto_sub im Container surfcam-mosquitto-1, dekodiert die
Rover-Pakete und zeigt alle 10 s Paketverlust, RSSI, SNR und Entfernung. Jedes Paket
wird zusätzlich in eine CSV-Datei geschrieben.

  python3 rangetest.py                          # live, CSV im aktuellen Ordner
  python3 rangetest.py --ref 47.1234567,8.1234567   # Entfernung zum Gateway/Kamerastandort
  python3 rangetest.py --stdin < mitschnitt.txt # Hex-Zeilen (mosquitto_sub -F %x) einlesen

Paketverlust wird aus den Sequenznummern berechnet (8 Bit). Bei Ausfällen über
≈ 25 s (Band B) bzw. ≈ 75 s (Band A) wird er aus der Empfangszeit geschätzt.
"""

import argparse
import csv
import math
import statistics
import struct
import subprocess
import sys
import time
from datetime import datetime

TOPIC = "eu868/gateway/+/event/up"
CONTAINER = "surfcam-mosquitto-1"
BANDS = {869525000: "A", 869850000: "B"}
PACKET_SIZES = {1: 19, 2: 22, 3: 22}  # Paketversion -> Länge
MHDR = 0xE0


# ------------------------------------------------------------------ Protobuf (minimal)

def _varint(buf, i):
    v = 0
    shift = 0
    while True:
        b = buf[i]
        i += 1
        v |= (b & 0x7F) << shift
        if not b & 0x80:
            return v, i
        shift += 7


def pb_fields(buf):
    """Felder einer Protobuf-Nachricht: {Nummer: [Werte]} (Bytes für LEN, int für VARINT/I32/I64)."""
    out = {}
    i = 0
    while i < len(buf):
        key, i = _varint(buf, i)
        num, wt = key >> 3, key & 7
        if wt == 0:
            v, i = _varint(buf, i)
        elif wt == 1:
            v = buf[i:i + 8]
            i += 8
        elif wt == 2:
            n, i = _varint(buf, i)
            v = buf[i:i + n]
            i += n
        elif wt == 5:
            v = buf[i:i + 4]
            i += 4
        else:
            raise ValueError("Wire-Type %d" % wt)
        out.setdefault(num, []).append(v)
    return out


def int32(v):
    v &= 0xFFFFFFFFFFFFFFFF
    return v - (1 << 64) if v >= 1 << 63 else v


def parse_uplink(raw):
    """UplinkFrame: 1 phy_payload, 4.1 Frequenz, 5.6 RSSI, 5.7 SNR."""
    f = pb_fields(raw)
    phy = f.get(1, [b""])[0]
    freq = rssi = snr = None
    if 4 in f:
        tx = pb_fields(f[4][0])
        freq = tx.get(1, [None])[0]
    if 5 in f:
        rx = pb_fields(f[5][0])
        if 6 in rx:
            rssi = int32(rx[6][0])
        if 7 in rx:
            snr = struct.unpack("<f", rx[7][0])[0]
    return phy, freq, rssi, snr


# ------------------------------------------------------------------ Rover-Paket

def decode_rover(p):
    if len(p) < 2 or p[0] != MHDR or PACKET_SIZES.get(p[1] >> 5) != len(p):
        return None
    w = int.from_bytes(p[3:7], "big")
    batt = w >> 27
    t = w & 0x7FFFFFF
    lat = int.from_bytes(p[7:11], "big", signed=True) / 1e7
    lon = int.from_bytes(p[11:15], "big", signed=True) / 1e7
    v3 = (p[1] >> 5) >= 3  # Byte 21: Bits 7..6 Tastenzähler, Bits 5..0 Neigung in 3 Grad (63 = unbekannt)
    if v3:
        tilt = (p[21] & 0x3F) * 3 if (p[21] & 0x3F) != 63 else None
    else:
        tilt = p[21] if len(p) >= 22 and p[21] != 255 else None
    return {
        "id": p[1] & 0x1F,
        "seq": p[2],
        "battery_mv": 3000 + 40 * batt if batt else 0,
        "time_ms": None if t == 0x7FFFFFF else t,
        "lat": lat,
        "lon": lon,
        "speed_mps": p[15] / 10,
        "course_deg": p[16] * 360 / 256,
        "fix": p[17] >> 6,
        "sats": p[17] & 0x3F,
        "hdop": p[18] / 10,
        # Version 2: Bewegung (255 = kein Sensor)
        "motion_rms_g": p[19] * 0.05 if len(p) >= 22 and p[19] != 255 else None,
        "motion_peak_g": p[20] * 0.1 if len(p) >= 22 and p[20] != 255 else None,
        "tilt_deg": tilt,
    }


def distance_m(lat1, lon1, lat2, lon2):
    r = 6371000.0
    p1, p2 = math.radians(lat1), math.radians(lat2)
    dp = p2 - p1
    dl = math.radians(lon2 - lon1)
    a = math.sin(dp / 2) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(dl / 2) ** 2
    return 2 * r * math.asin(math.sqrt(a))


# ------------------------------------------------------------------ Auswertung

class Stats:
    def __init__(self):
        self.reset()

    def reset(self):
        self.rx = 0
        self.lost = 0
        self.rssi = []
        self.snr = []
        self.dist = []
        self.fix = 0


def fmt_window(label, s, rate):
    exp = s.rx + s.lost
    loss = 100.0 * s.lost / exp if exp else 0.0
    parts = ["%s  %3d Pakete" % (label, s.rx), "Verlust %5.1f %%" % loss]
    if rate:
        parts.append("%4.1f/s" % rate)
    if s.rssi:
        parts.append("RSSI Ø %4.0f min %4d dBm" % (statistics.mean(s.rssi), min(s.rssi)))
    if s.snr:
        parts.append("SNR Ø %5.1f min %5.1f dB" % (statistics.mean(s.snr), min(s.snr)))
    if s.dist:
        parts.append("Distanz %4.0f m" % statistics.median(s.dist))
    parts.append("Fix %d/%d" % (s.fix, s.rx))
    return ", ".join(parts)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ref", help="Bezugspunkt lat,lon für die Entfernung (z. B. Gateway/Kamera)")
    ap.add_argument("--window", type=float, default=10.0, help="Auswertefenster in s (Standard 10)")
    ap.add_argument("--csv", help="CSV-Datei (Standard rangetest_JJJJMMTT_hhmmss.csv)")
    ap.add_argument("--stdin", action="store_true", help="Hex-Zeilen von stdin statt mosquitto_sub")
    ap.add_argument("-v", "--verbose", action="store_true", help="jedes Paket anzeigen")
    args = ap.parse_args()

    ref = None
    if args.ref:
        ref = tuple(float(x) for x in args.ref.split(","))

    csv_name = args.csv or datetime.now().strftime("rangetest_%Y%m%d_%H%M%S.csv")
    out = open(csv_name, "w", newline="")
    w = csv.writer(out)
    w.writerow(["rx_time", "band", "freq_hz", "id", "seq", "lost_before", "rssi_dbm", "snr_db", "fix",
                "sats", "hdop", "lat", "lon", "dist_m", "speed_mps", "course_deg", "gnss_utc_ms",
                "battery_mv", "motion_rms_g", "motion_peak_g", "tilt_deg"])

    if args.stdin:
        src = sys.stdin
        proc = None
    else:
        cmd = ["docker", "exec", CONTAINER, "mosquitto_sub", "-t", TOPIC, "-F", "%x"]
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, text=True, bufsize=1)
        src = proc.stdout

    print("Reichweitentest, CSV: %s (Strg+C beendet)" % csv_name, flush=True)
    win = Stats()
    tot = Stats()
    last_seq = {}
    last_rx = {}
    rate_est = {}
    win_start = time.time()
    band_seen = None

    try:
        for line in src:
            line = line.strip()
            if not line:
                continue
            try:
                phy, freq, rssi, snr = parse_uplink(bytes.fromhex(line))
            except (ValueError, IndexError):
                continue
            pkt = decode_rover(phy)
            if not pkt:
                continue  # fremde LoRaWAN-Geräte
            now = time.time()
            dev = pkt["id"]
            band = BANDS.get(freq, "?")
            if band != band_seen:
                print("Band %s (%.3f MHz)" % (band, (freq or 0) / 1e6), flush=True)
                band_seen = band

            lost = 0
            if dev in last_seq:
                lost = (pkt["seq"] - last_seq[dev] - 1) % 256
                dt = now - last_rx[dev]
                r = rate_est.get(dev)
                if r and dt > 0.8 * 256 / r:  # Sequenz evtl. übergelaufen: aus der Zeit schätzen
                    lost = max(lost, int(round(dt * r)) - 1)
                if 0 < dt < 2 and lost == 0:
                    rate_est[dev] = 0.9 * rate_est.get(dev, 1 / dt) + 0.1 / dt
            last_seq[dev] = pkt["seq"]
            last_rx[dev] = now

            dist = None
            if ref and pkt["fix"]:
                dist = distance_m(ref[0], ref[1], pkt["lat"], pkt["lon"])

            for s in (win, tot):
                s.rx += 1
                s.lost += lost
                if rssi is not None:
                    s.rssi.append(rssi)
                if snr is not None:
                    s.snr.append(snr)
                if dist is not None:
                    s.dist.append(dist)
                if pkt["fix"]:
                    s.fix += 1

            w.writerow([datetime.fromtimestamp(now).isoformat(timespec="milliseconds"), band, freq, dev,
                        pkt["seq"], lost, rssi, "%.1f" % snr if snr is not None else "", pkt["fix"],
                        pkt["sats"], pkt["hdop"], "%.7f" % pkt["lat"], "%.7f" % pkt["lon"],
                        "%.1f" % dist if dist is not None else "", pkt["speed_mps"],
                        "%.1f" % pkt["course_deg"], pkt["time_ms"] if pkt["time_ms"] is not None else "",
                        pkt["battery_mv"],
                        "%.2f" % pkt["motion_rms_g"] if pkt["motion_rms_g"] is not None else "",
                        "%.1f" % pkt["motion_peak_g"] if pkt["motion_peak_g"] is not None else "",
                        pkt["tilt_deg"] if pkt["tilt_deg"] is not None else ""])
            if args.verbose:
                mot = ""
                if pkt["motion_rms_g"] is not None:
                    mot = "  Bewegung %.2f g, Spitze %.1f g, Neigung %d°" % (
                        pkt["motion_rms_g"], pkt["motion_peak_g"], pkt["tilt_deg"])
                print("  seq %3d  verloren %2d  RSSI %4s  SNR %5s  Fix %d  Sat %2d  %s%s" % (
                    pkt["seq"], lost, rssi, "%.1f" % snr if snr is not None else "-", pkt["fix"],
                    pkt["sats"], "%.0f m" % dist if dist is not None else "", mot), flush=True)

            if now - win_start >= args.window:
                print(datetime.now().strftime("%H:%M:%S ") + fmt_window(
                    "", win, win.rx / (now - win_start)), flush=True)
                out.flush()
                win.reset()
                win_start = now
    except KeyboardInterrupt:
        pass
    finally:
        if proc:
            proc.terminate()
        out.close()
        print("\nGesamt: " + fmt_window("", tot, None))
        print("CSV: %s" % csv_name)


if __name__ == "__main__":
    main()
