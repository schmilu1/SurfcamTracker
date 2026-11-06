"""GNSS des Pi (Gateway-HAT) lesen, aufzeichnen und den Tracker relativ zur Kamera berechnen.

Das HAT-Modul versteht dieselben CASIC-Befehle wie der L76K im Tracker. Es wird bei jedem Start
auf dieselben Satellitensysteme wie der Tracker eingestellt (GPS + BeiDou + GLONASS, nur RMC + GGA),
nicht dauerhaft im Modul gespeichert. Standard 1 Hz bei 9600 Baud: Mit 10 Hz (115200 Baud) hing
das Modul am 06.11.2026 nach wenigen Sekunden und brauchte einen Stromunterbruch. Höhere Raten
nur mit --pi-gnss-rate.

Differenz-GPS im Positionsbereich (ohne Rohdaten):
  roh        = Tracker(t) - Kamerastandort            (Kamerastandort = Mittel des Trackers neben dem Pi)
  korrigiert = Tracker(t) - Pi(t) - Versatz           (Versatz = Mittel von Tracker - Pi neben dem Pi)
Gemeinsame Fehler beider Empfänger zur selben Zeit fallen in "korrigiert" heraus.
"""

import csv
import glob
import json
import math
import os
import threading
import time
from collections import deque
from datetime import datetime, timedelta, timezone

import serial

R_EARTH = 6371000.0
DAY_MS = 86400000


# ------------------------------------------------------------------ Hilfsfunktionen

def nmea_ok(line):
    """Prüfsumme einer NMEA-Zeile korrekt?"""
    if not line.startswith("$") or "*" not in line:
        return False
    body, _, cs = line[1:].partition("*")
    c = 0
    for ch in body.encode("ascii", "replace"):
        c ^= ch
    try:
        return c == int(cs[:2], 16)
    except ValueError:
        return False


def nmea_cmd(body):
    c = 0
    for ch in body.encode():
        c ^= ch
    return ("$%s*%02X\r\n" % (body, c)).encode()


def _coord(v, hemi):
    if not v:
        return None
    dot = v.index(".")
    deg = float(v[:dot - 2]) + float(v[dot - 2:]) / 60
    return -deg if hemi in ("S", "W") else deg


def _hms_ms(v):
    """'hhmmss.sss' -> ms seit Mitternacht."""
    return int(v[0:2]) * 3600000 + int(v[2:4]) * 60000 + round(float(v[4:]) * 1000)


def enu(lat, lon, lat0, lon0):
    """Ost/Nord in m relativ zu (lat0, lon0), flach genähert (genau genug für einige km)."""
    e = math.radians(lon - lon0) * R_EARTH * math.cos(math.radians(lat0))
    n = math.radians(lat - lat0) * R_EARTH
    return e, n


def polar(e, n):
    """-> (Entfernung m, Richtung ° ab Nord im Uhrzeigersinn)."""
    return math.hypot(e, n), (math.degrees(math.atan2(e, n)) + 360) % 360


class DailyCsv:
    """CSV-Datei pro UTC-Tag und Programmstart, alte Dateien werden nach keep_days gelöscht."""

    def __init__(self, folder, header, keep_days=30):
        self.folder = folder
        self.header = header
        self.keep_days = keep_days
        self.lock = threading.Lock()
        self.day = None
        self.f = None
        self.w = None
        self.path = None
        self.rows = 0
        self.flushed = 0.0
        os.makedirs(folder, exist_ok=True)

    def write(self, row, now=None):
        now = now or datetime.now(timezone.utc)
        with self.lock:
            if self.day != now.date():
                self._open(now)
            self.w.writerow(row)
            self.rows += 1
            if time.time() - self.flushed >= 5:  # spätestens alle 5 s auf die SD-Karte
                self.f.flush()
                self.flushed = time.time()

    def _open(self, now):
        if self.f:
            self.f.close()
        self.day = now.date()
        self.path = os.path.join(self.folder, now.strftime("%Y%m%d_%H%M%S") + ".csv")
        self.f = open(self.path, "a", newline="")
        self.w = csv.writer(self.f)
        self.w.writerow(self.header)
        self.f.flush()
        cutoff = time.time() - self.keep_days * 86400
        for p in glob.glob(os.path.join(self.folder, "*.csv")):
            if os.path.getmtime(p) < cutoff:
                os.remove(p)

    def flush(self):
        with self.lock:
            if self.f:
                self.f.flush()


# ------------------------------------------------------------------ GNSS des Pi

class PiGnss(threading.Thread):
    """Liest das GNSS-Modul des HAT, stellt es wie den Tracker ein und meldet jede Position."""

    CONFIG = ["PCAS04,7",                                   # GPS + BeiDou + GLONASS
              "PCAS03,1,0,0,0,1,0,0,0,0,0,,,0,0,,,,0"]      # nur GGA und RMC

    def __init__(self, port, rate_hz, on_fix, log=None):
        super().__init__(daemon=True)
        self.port = port
        self.rate_hz = rate_hz
        self.on_fix = on_fix
        self.log = log
        self.status = {"connected": False, "port": port, "baud": None, "msg": "startet"}
        self.last = None
        self.count = 0
        self.rate_window = deque(maxlen=50)
        self._date = None

    def _set(self, **kw):
        self.status.update(kw)

    def _sentences(self, ser, seconds, want=2):
        """Wartet auf want gültige NMEA-Sätze."""
        t0 = time.time()
        n = 0
        while time.time() - t0 < seconds:
            line = ser.readline().decode("ascii", "replace").strip()
            if nmea_ok(line):
                n += 1
                if n >= want:
                    return True
        return False

    def _configure(self):
        ser = None
        for baud in ((115200, 9600) if self.rate_hz > 1 else (9600, 115200)):
            ser = serial.Serial(self.port, baud, timeout=0.3)
            ser.reset_input_buffer()
            if self._sentences(ser, 2.0):
                break
            ser.close()
            ser = None
        if ser is None:
            raise IOError("keine NMEA-Daten auf %s" % self.port)
        for c in self.CONFIG:
            ser.write(nmea_cmd(c))
            time.sleep(0.1)
        if self.rate_hz > 1 and ser.baudrate != 115200:
            ser.write(nmea_cmd("PCAS01,5"))   # 115200 Baud (nicht gespeichert)
            time.sleep(0.2)
            ser.close()
            ser = serial.Serial(self.port, 115200, timeout=0.3)
            ser.reset_input_buffer()
            if not self._sentences(ser, 2.0):
                ser.close()
                ser = serial.Serial(self.port, 9600, timeout=0.3)
                self._set(msg="115200 Baud nicht übernommen, bleibe bei 9600 (max. 1 Hz)")
                self.rate_hz = 1
        if self.rate_hz > 1:
            ser.write(nmea_cmd("PCAS02,%d" % round(1000 / self.rate_hz)))
        elif ser.baudrate != 9600:
            ser.write(nmea_cmd("PCAS02,1000"))
        return ser

    def run(self):
        while True:
            try:
                if not os.path.exists(self.port):
                    self._set(connected=False, msg="%s nicht vorhanden" % self.port)
                    time.sleep(10)
                    continue
                ser = self._configure()
                self._set(connected=True, baud=ser.baudrate, msg="läuft")
                print("[pi-gnss] %s, %d Baud, %d Hz" % (self.port, ser.baudrate, self.rate_hz), flush=True)
                last_data = time.time()
                while True:
                    line = ser.readline().decode("ascii", "replace").strip()
                    if not line:
                        if time.time() - last_data > 5:
                            raise IOError("keine Daten mehr")
                        continue
                    last_data = time.time()
                    if nmea_ok(line):
                        self._parse(line)
            except Exception as e:
                self._set(connected=False, msg=str(e))
                print("[pi-gnss]", e, flush=True)
                time.sleep(5)

    def _parse(self, line):
        f = line.split("*")[0].split(",")
        typ = f[0][3:]
        if typ == "RMC" and len(f) > 9 and len(f[9]) == 6:
            self._date = f[9]  # ddmmyy
        elif typ == "GGA" and len(f) > 9 and f[1]:
            q = int(f[6] or 0)
            ms = _hms_ms(f[1])
            fix = {"utc_ms": ms, "q": q, "sats": int(f[7] or 0),
                   "hdop": float(f[8]) if f[8] else None,
                   "lat": _coord(f[2], f[3]) if q else None, "lon": _coord(f[4], f[5]) if q else None,
                   "alt": float(f[9]) if q and f[9] else None, "t": None, "date": None}
            if self._date:
                d = datetime.strptime(self._date, "%d%m%y").replace(tzinfo=timezone.utc)
                fix["date"] = d.date().isoformat()
                fix["t"] = d.timestamp() + ms / 1000
            self.last = fix
            self.count += 1
            self.rate_window.append(time.time())
            if self.log and fix["date"]:
                self.log.write([fix["date"], "%02d:%02d:%06.3f" % (ms // 3600000, ms // 60000 % 60, ms % 60000 / 1000),
                                ms, "%.7f" % fix["lat"] if q else "", "%.7f" % fix["lon"] if q else "",
                                fix["alt"] if fix["alt"] is not None else "", q, fix["sats"],
                                fix["hdop"] if fix["hdop"] is not None else ""])
            self.on_fix(fix)

    def info(self):
        w = list(self.rate_window)
        hz = (len(w) - 1) / (w[-1] - w[0]) if len(w) > 2 and w[-1] > w[0] else 0
        if w and time.time() - w[-1] > 3:
            hz = 0
        return dict(self.status, rate_set=self.rate_hz, hz=round(hz, 1), last=self.last, count=self.count)


# ------------------------------------------------------------------ Tracker relativ zur Kamera

def abs_ms(date_iso, utc_ms):
    d = datetime.fromisoformat(date_iso).replace(tzinfo=timezone.utc)
    return int(d.timestamp() * 1000) + utc_ms


class Relative:
    """Pi-Mittelwert, Kamerastandort, roh/korrigiert und Statistik."""

    MOVED_M = 50          # Pi weiter weg vom Mittelwert: neu beginnen (Pi wurde umgestellt)
    BASE_MAX_N = 36000    # Mittelwert über höchstens ≈ 1 h bei 10 Hz, danach gleitend

    def __init__(self, state_path):
        self.lock = threading.Lock()
        self.state_path = state_path
        self.base = None          # {"lat", "lon", "n"}
        self.cam = None           # {"lat", "lon", "bias_e", "bias_n", "n", "n_pairs", "set_at"}
        self.calib = None         # laufende Kalibrierung
        self.pi_fixes = {}        # abs. ms (auf 100 ms gerundet) -> (lat, lon)
        self.pi_order = deque()
        self.history = deque(maxlen=20000)  # (t, raw_e, raw_n, corr_e, corr_n)
        self.last = None
        self.last_calib_msg = ""
        self._load()

    # ---- Speichern
    def _load(self):
        try:
            with open(self.state_path) as f:
                s = json.load(f)
            self.base = s.get("base")
            self.cam = s.get("cam")
        except (OSError, ValueError):
            pass

    def _save(self):
        try:
            os.makedirs(os.path.dirname(self.state_path), exist_ok=True)
            tmp = self.state_path + ".tmp"
            with open(tmp, "w") as f:
                json.dump({"base": self.base, "cam": self.cam}, f, indent=1)
            os.replace(tmp, self.state_path)
        except OSError as e:
            print("[position] speichern:", e, flush=True)

    # ---- Pi
    def on_pi_fix(self, fix):
        if not fix["q"] or fix["lat"] is None or fix["t"] is None:
            return
        with self.lock:
            b = self.base
            if b and polar(*enu(fix["lat"], fix["lon"], b["lat"], b["lon"]))[0] > self.MOVED_M:
                print("[position] Pi > %d m vom Mittelwert: neu beginnen, Kamerastandort gelöscht" % self.MOVED_M,
                      flush=True)
                self.base = None
                self.cam = None
                b = None
            if not b:
                self.base = b = {"lat": fix["lat"], "lon": fix["lon"], "n": 0}
            n = min(b["n"] + 1, self.BASE_MAX_N)
            b["lat"] += (fix["lat"] - b["lat"]) / n
            b["lon"] += (fix["lon"] - b["lon"]) / n
            b["n"] = n
            key = round(fix["t"] * 10) * 100
            self.pi_fixes[key] = (fix["lat"], fix["lon"])
            self.pi_order.append(key)
            while len(self.pi_order) > 3000:  # 5 min bei 10 Hz, 50 min bei 1 Hz
                self.pi_fixes.pop(self.pi_order.popleft(), None)
            if b["n"] % 600 == 1:
                self._save()

    def _pi_at(self, key):
        """Pi-Position zur Zeit key (ms): genau, zwischen zwei Positionen interpoliert (Abstand ≤ 1,1 s)
        oder die letzte davor, wenn sie höchstens 1,5 s alt ist (die nächste ist evtl. noch nicht da)."""
        p = self.pi_fixes.get(key)
        if p:
            return p
        after = None
        for k in reversed(self.pi_order):
            if k <= key:
                if after is not None and after - k <= 1100:
                    a, b = self.pi_fixes[k], self.pi_fixes[after]
                    f = (key - k) / (after - k)
                    return (a[0] + (b[0] - a[0]) * f, a[1] + (b[1] - a[1]) * f)
                return self.pi_fixes[k] if key - k <= 1500 else None
            after = k
            if after - key > 5000:
                break
        return None

    def reset_base(self):
        with self.lock:
            self.base = None
            self._save()

    # ---- Tracker
    def on_packet(self, p):
        if not p.get("fix") or p.get("utc_ms") is None:
            return
        rx = datetime.fromtimestamp(p["rx"], timezone.utc)
        t = rx.replace(hour=0, minute=0, second=0, microsecond=0) + timedelta(milliseconds=p["utc_ms"])
        if (t - rx).total_seconds() > 43200:
            t -= timedelta(days=1)
        elif (rx - t).total_seconds() > 43200:
            t += timedelta(days=1)
        key = round(t.timestamp() * 10) * 100
        with self.lock:
            pi = self._pi_at(key)
            d_pi = enu(p["lat"], p["lon"], pi[0], pi[1]) if pi else None  # Tracker - Pi zur selben Zeit
            c = self.calib
            if c:
                c["lat"] += p["lat"]
                c["lon"] += p["lon"]
                c["n"] += 1
                if d_pi:
                    c["de"] += d_pi[0]
                    c["dn"] += d_pi[1]
                    c["n_pairs"] += 1
                if time.time() >= c["end"]:
                    self._finish_calib()
            raw = corr = None
            if self.cam:
                raw = enu(p["lat"], p["lon"], self.cam["lat"], self.cam["lon"])
                if d_pi and self.cam.get("n_pairs"):
                    corr = (d_pi[0] - self.cam["bias_e"], d_pi[1] - self.cam["bias_n"])
            else:
                if self.base:
                    raw = enu(p["lat"], p["lon"], self.base["lat"], self.base["lon"])
                corr = d_pi
            self.last = {"t": key / 1000, "raw": raw, "corr": corr, "pi_match": bool(pi)}
            if raw or corr:
                self.history.append((key / 1000, raw, corr))

    # ---- Kalibrierung (Tracker neben der Kamera)
    def start_calib(self, seconds):
        with self.lock:
            self.calib = {"start": time.time(), "end": time.time() + seconds, "seconds": seconds,
                          "lat": 0.0, "lon": 0.0, "n": 0, "de": 0.0, "dn": 0.0, "n_pairs": 0}

    def cancel_calib(self):
        with self.lock:
            self.calib = None

    def _finish_calib(self):
        c = self.calib
        self.calib = None
        if c["n"] < 5:
            self.last_calib_msg = "zu wenige Tracker-Positionen mit Fix (%d)" % c["n"]
            return
        self.cam = {"lat": c["lat"] / c["n"], "lon": c["lon"] / c["n"], "n": c["n"],
                    "n_pairs": c["n_pairs"],
                    "bias_e": c["de"] / c["n_pairs"] if c["n_pairs"] else 0.0,
                    "bias_n": c["dn"] / c["n_pairs"] if c["n_pairs"] else 0.0,
                    "set_at": datetime.now(timezone.utc).isoformat(timespec="seconds")}
        self.history.clear()
        self.last_calib_msg = "Kamerastandort aus %d Positionen (%d mit Pi-GPS)" % (c["n"], c["n_pairs"])
        self._save()

    def clear_cam(self):
        with self.lock:
            self.cam = None
            self.history.clear()
            self._save()

    # ---- Ausgabe
    @staticmethod
    def _stats(vs):
        vs = [v for v in vs if v]
        if not vs:
            return None
        d = sorted(math.hypot(e, n) for e, n in vs)
        me = sum(e for e, _ in vs) / len(vs)
        mn = sum(n for _, n in vs) / len(vs)
        spread = math.sqrt(sum((e - me) ** 2 + (n - mn) ** 2 for e, n in vs) / len(vs))
        return {"n": len(vs), "mean_d": round(sum(d) / len(d), 2), "p95_d": round(d[int(0.95 * (len(d) - 1))], 2),
                "mean_e": round(me, 2), "mean_n": round(mn, 2), "spread": round(spread, 2)}

    def info(self, window_s=300, max_points=1500):
        with self.lock:
            now = time.time()
            if self.calib and now >= self.calib["end"]:
                self._finish_calib()
            h = [x for x in self.history if x[0] >= now - window_s]
            step = max(1, len(h) // max_points)
            pts = [[round(v, 2) if v is not None else None for v in ((r or (None, None)) + (c or (None, None)))]
                   for _, r, c in h[::step]]
            last = None
            if self.last:
                last = {"age": round(now - self.last["t"], 1), "pi_match": self.last["pi_match"]}
                for k in ("raw", "corr"):
                    v = self.last[k]
                    last[k] = None if not v else dict(zip(("d", "brg"), (round(x, 1) for x in polar(*v))),
                                                      e=round(v[0], 2), n=round(v[1], 2))
            base = None
            if self.base:
                # Streuung der Pi-Positionen der letzten 5 min um den Mittelwert
                ds = [enu(la, lo, self.base["lat"], self.base["lon"]) for la, lo in self.pi_fixes.values()]
                base = {"lat": round(self.base["lat"], 7), "lon": round(self.base["lon"], 7), "n": self.base["n"],
                        "spread": round(math.sqrt(sum(e * e + n * n for e, n in ds) / len(ds)), 2) if ds else None}
            calib = None
            if self.calib:
                calib = {"remaining": round(max(0, self.calib["end"] - now)), "seconds": self.calib["seconds"],
                         "n": self.calib["n"], "n_pairs": self.calib["n_pairs"]}
            return {"base": base, "cam": self.cam, "calib": calib, "calib_msg": self.last_calib_msg,
                    "last": last, "window_s": window_s,
                    "stats": {"raw": self._stats([r for _, r, _ in h]), "corr": self._stats([c for _, _, c in h])},
                    "points": pts}
