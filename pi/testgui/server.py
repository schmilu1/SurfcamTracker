#!/usr/bin/env python3
"""SurfCam Test-GUI: Webserver für Live-Daten, Tracker-Einstellungen und Gateway.

  python3 server.py                      # Port 8080, MQTT localhost:1883, Tracker automatisch
  python3 server.py --mqtt 192.168.8.125 --tracker /dev/cu.usbserial-XXXX   # z. B. auf dem Mac

Live-Daten kommen über MQTT von der Gateway Bridge. Der Tracker wird über USB (seriell,
AT-Befehle) angesprochen, sobald er am Pi steckt. Abhängigkeiten: Python 3.9+, pyserial.
"""

import argparse
import csv
import glob
import io
import json
import os
import queue
import re
import subprocess
import sys
import threading
import time
from collections import deque
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, unquote, urlparse

import serial

import moblin
import pool
import position
import power
import uplink

HERE = os.path.dirname(os.path.abspath(__file__))
STATIC = os.path.join(HERE, "static")
REPO = os.path.dirname(os.path.dirname(HERE))
# Hilfe-Tab: README-Dateien aus dem Repo (id, Titel, Pfad relativ zum Repo)
HELP_DOCS = [
    ("gui", "Test-GUI", "pi/testgui/README.md"),
    ("hotspot", "Hotspot", "pi/hotspot/README.md"),
    ("tracker", "Tracker-Firmware", "firmware/rover/README.md"),
    ("gateway", "Gateway", "docs/gateway.md"),
    ("dgps", "DGPS-Test", "tools/dgps/README.md"),
    ("pool", "Surf-Pool", "docs/pool.md"),
    ("power", "Stromverbrauch", "docs/power.md"),
]
RX_FIELDS = ["band", "id", "seq", "lost", "rssi", "snr", "fix", "sats", "hdop", "lat", "lon", "speed", "course",
             "utc_ms", "battery_mv", "motion_rms", "motion_peak", "tilt", "button"]
# in main() gesetzt
rx_log = None      # Aufzeichnung aller empfangenen Pakete
relative = None    # position.Relative
pi_gnss = None     # position.PiGnss
pi_log = None      # Aufzeichnung des Pi-GNSS
pool_mode = None   # pool.Pool (Video-Puffer)
power_meter = None  # power.Power (Stromverbrauch des Pi)
DATA_KINDS = ("rx", "pi_gnss")
DEFAULT_BAND_SH = os.path.expanduser("~/sx1302_hal_rpi5-master/surfcam/band.sh")
SESSION_RE = re.compile(r"^(\d{6}_\d{6})\s+(\d+) Segmente\s+(\d+) Bytes\s+(\d+) Positionen(\s+\(aktiv\))?")


# ------------------------------------------------------------------ Ereignisse an Browser (SSE)

class Broadcaster:
    def __init__(self):
        self.lock = threading.Lock()
        self.clients = set()

    def subscribe(self):
        q = queue.Queue(maxsize=1000)
        with self.lock:
            self.clients.add(q)
        return q

    def unsubscribe(self, q):
        with self.lock:
            self.clients.discard(q)

    def send(self, event, data):
        msg = "event: %s\ndata: %s\n\n" % (event, json.dumps(data, separators=(",", ":")))
        with self.lock:
            for q in list(self.clients):
                try:
                    q.put_nowait(msg)
                except queue.Full:
                    pass  # langsamer Client: Nachricht verwerfen


events = Broadcaster()


# ------------------------------------------------------------------ Funkpakete

class PacketStore:
    """Letzte Pakete und Statistik pro Gerät (Verlust aus Sequenznummern)."""

    def __init__(self, maxlen=5000):
        self.lock = threading.Lock()
        self.packets = deque(maxlen=maxlen)
        self.last = {}  # Geräte-ID -> (seq, rx_time)
        self.button = {}  # Geräte-ID -> letzter Tastenzähler (Paket Version 3)
        self.mqtt = {"ok": False, "msg": "startet"}
        self.foreign = 0

    def on_status(self, ok, msg):
        self.mqtt = {"ok": ok, "msg": msg}
        print("[mqtt]", msg, flush=True)
        events.send("mqtt", self.mqtt)

    def on_message(self, topic, payload):
        phy, freq, rssi, snr, gw = uplink.parse_uplink(payload)
        p = uplink.decode_rover(phy)
        if not p:
            self.foreign += 1
            return
        now = time.time()
        with self.lock:
            lost = 0
            prev = self.last.get(p["id"])
            if prev:
                lost = (p["seq"] - prev[0] - 1) % 256
                if lost > 200 and now - prev[1] < 5:
                    lost = 0  # Rover neu gestartet (Sequenz beginnt bei 0)
            restarted = False
            if prev and lost == 0 and (p["seq"] - prev[0] - 1) % 256 > 200:
                restarted = True
            self.last[p["id"]] = (p["seq"], now)
            presses = self._button(p, prev, now, lost, restarted)
            p.update({"rx": now, "lost": lost, "freq": freq, "band": uplink.BANDS.get(freq, "?"),
                      "rssi": rssi, "snr": snr, "gateway": gw})
            self.packets.append(p)
        events.send("packet", p)
        if rx_log:
            rx_log.write([datetime.fromtimestamp(now, timezone.utc).isoformat(timespec="milliseconds")] +
                         [p.get(f) if p.get(f) is not None else "" for f in RX_FIELDS])
        if relative:
            relative.on_packet(p)
        if presses:
            self._on_button(p, presses)

    def _button(self, p, prev, now, lost, restarted):
        """Anzahl neuer kurzer Tastendrücke seit dem letzten Paket (Zähler 0..3 im Paket).
        Nach Neustart des Trackers (Zähler wieder 0), langer Pause oder vielen verlorenen Paketen
        wird nur neu synchronisiert, damit kein falscher Clip entsteht."""
        b = p.get("button")
        if b is None:
            return 0
        old = self.button.get(p["id"])
        self.button[p["id"]] = b
        if old is None or restarted or not prev or now - prev[1] > 10 or lost > 30:
            return 0
        return (b - old) % 4

    def _on_button(self, p, presses):
        src = "Taste Tracker %d" % p["id"]
        print("[taste] Tracker %d: %d× gedrückt" % (p["id"], presses), flush=True)
        if pool_mode:
            ok, text = pool_mode.clip(source=src)
            if not ok:
                pool_mode.last = {"ok": False, "name": None, "source": src, "t": time.time(), "msg": text}
                events.send("pool", pool_mode.info())
        events.send("button", {"id": p["id"], "presses": presses})

    def recent(self, seconds=600, limit=3000):
        cutoff = time.time() - seconds
        with self.lock:
            return [p for p in self.packets if p["rx"] >= cutoff][-limit:]

    def csv(self):
        out = io.StringIO()
        with self.lock:
            rows = list(self.packets)
        fields = ["rx", "band", "freq", "id", "seq", "lost", "rssi", "snr", "fix", "sats", "hdop", "lat", "lon",
                  "speed", "course", "utc_ms", "battery_mv", "motion_rms", "motion_peak", "tilt", "button"]
        w = csv.writer(out)
        w.writerow(["rx_time"] + fields[1:])
        for p in rows:
            w.writerow([datetime.fromtimestamp(p["rx"]).isoformat(timespec="milliseconds")] +
                       [p.get(f) if p.get(f) is not None else "" for f in fields[1:]])
        return out.getvalue()


packets = PacketStore()


# ------------------------------------------------------------------ Tracker (USB, AT-Befehle)

class Tracker(threading.Thread):
    """Hält die serielle Verbindung zum Tracker, verbindet automatisch neu.

    Antworten auf Befehle werden bis zur Zeile OK / ERROR gesammelt. Alle anderen Zeilen
    (Bootmeldungen, [log], [gnss] ...) gehen in die Konsole."""

    def __init__(self, port_hint):
        super().__init__(daemon=True)
        self.port_hint = port_hint
        self.ser = None
        self.port = None
        self.cmd_lock = threading.Lock()
        self.active = None  # Queue der laufenden Antwort
        self.console = deque(maxlen=400)
        self.status = None
        self.status_time = 0
        self.connected_since = 0

    def find_port(self):
        if self.port_hint:
            return self.port_hint if os.path.exists(self.port_hint) else None
        cands = sorted(glob.glob("/dev/serial/by-id/*")) + sorted(glob.glob("/dev/ttyUSB*")) + \
            sorted(glob.glob("/dev/ttyACM*")) + sorted(glob.glob("/dev/cu.usbserial*"))
        return cands[0] if cands else None

    def info(self):
        return {"connected": self.ser is not None, "port": self.port, "status": self.status,
                "status_age": round(time.time() - self.status_time, 1) if self.status_time else None}

    def _set_connected(self, ser, port):
        self.ser, self.port = ser, port
        if ser is None:
            self.status = None
        events.send("tracker", self.info())

    def _console(self, line):
        self.console.append(line)
        events.send("console", line)

    def run(self):
        while True:
            port = self.find_port()
            if not port:
                time.sleep(2)
                continue
            try:
                ser = serial.Serial()
                ser.port, ser.baudrate, ser.timeout = port, 115200, 0.2
                ser.dtr = False  # möglichst kein Reset beim Öffnen (Auto-Reset-Schaltung)
                ser.rts = False
                ser.open()
            except Exception as e:
                print("[tracker] %s: %s" % (port, e), flush=True)
                time.sleep(3)
                continue
            print("[tracker] verbunden:", port, flush=True)
            self.connected_since = time.time()
            self._set_connected(ser, port)
            self._console("--- verbunden: %s ---" % port)
            buf = b""
            try:
                while True:
                    chunk = ser.read(512)
                    if not chunk:
                        continue
                    buf += chunk
                    while b"\n" in buf:
                        raw, buf = buf.split(b"\n", 1)
                        line = raw.decode("utf-8", "replace").rstrip("\r")
                        q = self.active
                        if q is not None:
                            q.put(line)
                        elif line:
                            self._console(line)
            except Exception as e:
                print("[tracker] getrennt:", e, flush=True)
            try:
                ser.close()
            except Exception:
                pass
            self._set_connected(None, None)
            self._console("--- getrennt ---")
            if self.active is not None:
                self.active.put(None)
            time.sleep(2)

    def command(self, cmd, timeout=5.0, on_line=None, idle_timeout=None):
        """Befehl senden, Antwortzeilen bis OK/ERROR liefern. on_line: für lange Ausgaben (Streaming).
        Rückgabe: (ok, zeilen)."""
        if self.ser is None:
            raise RuntimeError("Tracker nicht verbunden")
        if not self.cmd_lock.acquire(timeout=timeout):
            raise RuntimeError("Tracker beschäftigt")
        q = queue.Queue()
        lines = []
        try:
            self.active = q
            self.ser.write((cmd.strip() + "\r\n").encode())
            if not cmd.upper().startswith(("AT+JSON",)):
                self._console("> " + cmd.strip())
            deadline = time.time() + timeout
            last = time.time()
            while True:
                limit = (last + idle_timeout) if idle_timeout else deadline
                wait = limit - time.time()
                if wait <= 0:
                    return False, lines + ["(Zeitüberschreitung)"]
                try:
                    line = q.get(timeout=wait)
                except queue.Empty:
                    continue
                if line is None:
                    return False, lines + ["(Verbindung getrennt)"]
                last = time.time()
                if line == "OK":
                    return True, lines
                if line.startswith("ERROR"):
                    return False, lines + [line]
                if on_line:
                    on_line(line)
                else:
                    lines.append(line)
        finally:
            self.active = None
            self.cmd_lock.release()
            if not cmd.upper().startswith("AT+JSON"):
                for l in lines:
                    self._console(l)

    def refresh_status(self):
        ok, lines = self.command("AT+JSON", timeout=3)
        for l in lines:
            if l.startswith("{"):
                self.status = json.loads(l)
                self.status_time = time.time()
                events.send("tracker", self.info())
                return self.status
        raise RuntimeError("keine JSON-Antwort: %s" % " / ".join(lines[-3:]))


tracker = None


def status_poller():
    """Alle 2 s den Tracker-Status holen (nur wenn verbunden und nicht beschäftigt)."""
    while True:
        time.sleep(2)
        if tracker.ser is None or tracker.cmd_lock.locked():
            continue
        if time.time() - tracker.connected_since < 3:
            continue  # nach dem Verbinden startet der Tracker evtl. neu
        try:
            tracker.refresh_status()
        except Exception:
            pass


# ------------------------------------------------------------------ Gateway

def gateway_info(band_sh):
    link = os.path.join(os.path.dirname(band_sh), "global_conf_active.json")
    band = None
    if os.path.islink(link):
        m = re.search(r"global_conf_([AB])\.json", os.readlink(link))
        band = m.group(1) if m else None
    try:
        active = subprocess.run(["systemctl", "is-active", "lora-pkt-fwd"], capture_output=True,
                                text=True, timeout=5).stdout.strip()
    except Exception as e:
        active = "unbekannt (%s)" % e
    return {"band": band, "service": active, "band_sh": os.path.exists(band_sh)}


def gateway_set_band(band_sh, band):
    # band.sh läuft als normaler Benutzer; nur "systemctl restart lora-pkt-fwd" braucht sudo
    # (ohne Passwort freigegeben, siehe README)
    r = subprocess.run([band_sh, band], capture_output=True, text=True, timeout=30)
    return r.returncode == 0, (r.stdout + r.stderr).strip()


# ------------------------------------------------------------------ HTTP

class Handler(BaseHTTPRequestHandler):
    server_version = "SurfCamTestGUI/1.0"
    band_sh = DEFAULT_BAND_SH

    def log_message(self, fmt, *args):
        pass  # kein Zugriffslog

    def _json(self, data, code=200):
        body = json.dumps(data).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _error(self, msg, code=400):
        self._json({"ok": False, "error": msg}, code)

    def _body(self):
        n = int(self.headers.get("Content-Length") or 0)
        if not n:
            return {}
        return json.loads(self.rfile.read(n).decode())

    def _static(self, name, ctype):
        path = os.path.join(STATIC, name)
        try:
            with open(path, "rb") as f:
                body = f.read()
        except OSError:
            return self._error("nicht gefunden", 404)
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-cache")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        url = urlparse(self.path)
        path = url.path
        try:
            if path in ("/", "/index.html"):
                return self._static("index.html", "text/html; charset=utf-8")
            # Bibliotheken lokal (Leaflet), damit die Seite auch ohne Internet läuft (Hotspot)
            m = re.match(r"^/vendor/([a-z0-9-]+/(?:images/)?[A-Za-z0-9._-]+\.(js|css|png))$", path)
            if m:
                ctype = {"js": "text/javascript", "css": "text/css", "png": "image/png"}[m.group(2)]
                return self._static("vendor/" + m.group(1), ctype)
            if path == "/api/state":
                secs = int(parse_qs(url.query).get("seconds", ["600"])[0])
                return self._json({"tracker": tracker.info(), "mqtt": packets.mqtt,
                                   "packets": packets.recent(secs), "foreign": packets.foreign,
                                   "gateway": gateway_info(self.band_sh),
                                   "console": list(tracker.console)[-200:], "server_time": time.time()})
            if path == "/api/events":
                return self._sse()
            if path == "/api/packets.csv":
                body = packets.csv().encode()
                self.send_response(200)
                self.send_header("Content-Type", "text/csv; charset=utf-8")
                self.send_header("Content-Disposition", 'attachment; filename="pakete_%s.csv"' %
                                 datetime.now().strftime("%Y%m%d_%H%M%S"))
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                return self.wfile.write(body)
            if path == "/api/position":
                win = int(parse_qs(url.query).get("window", ["300"])[0])
                return self._json({"pi": pi_gnss.info() if pi_gnss else None,
                                   "rel": relative.info(max(10, min(win, 7200)))})
            if path == "/api/data":
                files = {}
                for kind in DATA_KINDS:
                    folder = os.path.join(self.data_dir, kind)
                    files[kind] = [{"name": os.path.basename(f), "bytes": os.path.getsize(f),
                                    "mtime": os.path.getmtime(f)}
                                   for f in sorted(glob.glob(os.path.join(folder, "*.csv")), reverse=True)]
                return self._json(files)
            m = re.match(r"^/api/data/(rx|pi_gnss)/(\d{8}_\d{6}\.csv)$", path)
            if m:
                if rx_log:
                    rx_log.flush()
                if pi_log:
                    pi_log.flush()
                fp = os.path.join(self.data_dir, m.group(1), m.group(2))
                try:
                    with open(fp, "rb") as f:
                        body = f.read()
                except OSError:
                    return self._error("nicht gefunden", 404)
                self.send_response(200)
                self.send_header("Content-Type", "text/csv; charset=utf-8")
                self.send_header("Content-Disposition", 'attachment; filename="%s_%s"' % (m.group(1), m.group(2)))
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
                return
            if path == "/api/power":
                since = float(parse_qs(url.query).get("since", ["0"])[0])
                return self._json(power_meter.info(since))
            if path == "/api/pool":
                if not pool_mode:
                    return self._json({"state": "ausgeschaltet (--pool-port 0)", "clips": []})
                return self._json(dict(pool_mode.info(), clips=pool_mode.clips()))
            m = re.match(r"^/api/pool/clips/([A-Za-z0-9_]+\.mp4)$", path)
            if m:
                fp = pool_mode.clip_path(m.group(1)) if pool_mode else None
                if not fp:
                    return self._error("nicht gefunden", 404)
                return self._file_range(fp, "video/mp4", m.group(1), "dl" in parse_qs(url.query))
            if path == "/api/pool/phone/preview":
                data, ctype = pool_mode.phone.preview_image() if pool_mode and pool_mode.phone else (None, None)
                if not data:
                    return self._error("kein Vorschaubild", 404)
                self.send_response(200)
                self.send_header("Content-Type", ctype)
                self.send_header("Content-Length", str(len(data)))
                self.send_header("Cache-Control", "no-store")
                self.end_headers()
                return self.wfile.write(data)
            if path == "/api/help":
                docs = []
                for doc_id, title, rel in HELP_DOCS:
                    try:
                        with open(os.path.join(REPO, rel), encoding="utf-8") as f:
                            md = f.read()
                    except OSError:
                        md = "_Datei %s nicht gefunden._" % rel
                    docs.append({"id": doc_id, "title": title, "path": rel, "md": md})
                return self._json({"docs": docs})
            if path == "/api/gateway":
                return self._json(gateway_info(self.band_sh))
            if path == "/api/tracker/logs":
                ok, lines = tracker.command("AT+LOG", timeout=10)
                sessions = []
                for l in lines:
                    m = SESSION_RE.match(l)
                    if m:
                        sessions.append({"name": m.group(1), "segments": int(m.group(2)),
                                         "bytes": int(m.group(3)), "records": int(m.group(4)),
                                         "active": bool(m.group(5))})
                summary = next((l for l in lines if "Sessions" in l), "")
                return self._json({"ok": ok, "sessions": sessions, "summary": summary})
            m = re.match(r"^/api/tracker/logs/(\d{6}_\d{6})\.csv$", path)
            if m:
                return self._log_download(m.group(1))
            return self._error("nicht gefunden", 404)
        except RuntimeError as e:
            return self._error(str(e), 409)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def do_POST(self):
        path = urlparse(self.path).path
        try:
            body = self._body()
            if path == "/api/position/calibrate":
                relative.start_calib(max(10, min(int(body.get("seconds", 60)), 600)))
                return self._json({"ok": True})
            if path == "/api/position/cancel":
                relative.cancel_calib()
                return self._json({"ok": True})
            if path == "/api/position/clear_cam":
                relative.clear_cam()
                return self._json({"ok": True})
            if path == "/api/position/reset_base":
                relative.reset_base()
                return self._json({"ok": True})
            if path.startswith("/api/pool/") and not pool_mode:
                return self._error("Pool-Modus ausgeschaltet", 409)
            if path == "/api/pool/clip":
                ok, text = pool_mode.clip(int(body["seconds"]) if body.get("seconds") else None, "GUI")
                return self._json({"ok": ok, "msg": text})
            if path == "/api/pool/settings":
                pool_mode.set_clip_seconds(int(body.get("clip_seconds", 0)))
                return self._json({"ok": True})
            if path == "/api/pool/phone":
                if not pool_mode.phone:
                    return self._error("Fernsteuerung ausgeschaltet (--moblin-port 0)", 409)
                ok, text = pool_mode.phone.action(str(body.get("action", "")), body.get("value"))
                return self._json({"ok": ok, "error": text} if not ok else {"ok": True})
            if path == "/api/pool/delete":
                pool_mode.delete(str(body.get("name", "")))
                return self._json({"ok": True})
            if path == "/api/tracker/status":
                return self._json({"ok": True, "status": tracker.refresh_status()})
            if path == "/api/tracker/cmd":
                cmd = str(body.get("cmd", "")).strip()
                if not cmd.upper().startswith("AT"):
                    return self._error("Befehl muss mit AT beginnen")
                if cmd.upper().startswith("AT+LOGDUMP"):
                    return self._error("Log bitte über die Log-Seite herunterladen")
                timeout = 15 if cmd.upper().startswith(("AT+MOTION", "AT+BTN")) else 8
                ok, lines = tracker.command(cmd, timeout=timeout)
                if cmd.upper() in ("ATZ", "AT+FDR"):
                    tracker.connected_since = time.time()
                try:
                    if cmd.upper() not in ("ATZ", "AT+FDR", "AT+SLEEP"):
                        tracker.refresh_status()
                except Exception:
                    pass
                return self._json({"ok": ok, "lines": lines})
            if path == "/api/gateway/band":
                band = str(body.get("band", "")).upper()
                if band not in ("A", "B"):
                    return self._error("Band A oder B")
                result = {"ok": True}
                if body.get("tracker"):
                    ok, lines = tracker.command("AT+BAND=" + band, timeout=8)
                    result["tracker"] = {"ok": ok, "lines": lines}
                    result["ok"] = ok
                    try:
                        tracker.refresh_status()
                    except Exception:
                        pass
                if body.get("gateway", True):
                    ok, out = gateway_set_band(self.band_sh, band)
                    result["gateway"] = {"ok": ok, "output": out}
                    result["ok"] = result["ok"] and ok
                    events.send("gateway", gateway_info(self.band_sh))
                return self._json(result)
            return self._error("nicht gefunden", 404)
        except RuntimeError as e:
            return self._error(str(e), 409)
        except (ValueError, json.JSONDecodeError) as e:
            return self._error("ungültige Anfrage: %s" % e)

    def _file_range(self, fp, ctype, name, download):
        """Datei in Stücken senden, mit HTTP-Range (Safari spielt Videos nur so ab)."""
        size = os.path.getsize(fp)
        start, end = 0, size - 1
        m = re.match(r"bytes=(\d*)-(\d*)$", self.headers.get("Range") or "")
        if m and (m.group(1) or m.group(2)):
            if m.group(1):
                start = int(m.group(1))
                end = min(int(m.group(2)), size - 1) if m.group(2) else size - 1
            else:
                start = max(0, size - int(m.group(2)))
            if start > end:
                self.send_response(416)
                self.send_header("Content-Range", "bytes */%d" % size)
                self.end_headers()
                return
            self.send_response(206)
            self.send_header("Content-Range", "bytes %d-%d/%d" % (start, end, size))
        else:
            self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Content-Length", str(end - start + 1))
        if download:
            self.send_header("Content-Disposition", 'attachment; filename="%s"' % name)
        self.end_headers()
        with open(fp, "rb") as f:
            f.seek(start)
            left = end - start + 1
            while left > 0:
                chunk = f.read(min(1 << 20, left))
                if not chunk:
                    break
                self.wfile.write(chunk)
                left -= len(chunk)

    def _sse(self):
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Accel-Buffering", "no")
        self.end_headers()
        q = events.subscribe()
        try:
            self.wfile.write(b": verbunden\n\n")
            self.wfile.flush()
            while True:
                try:
                    msg = q.get(timeout=15)
                except queue.Empty:
                    msg = ": ping\n\n"
                self.wfile.write(msg.encode())
                self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError, OSError):
            pass
        finally:
            events.unsubscribe(q)

    def _log_download(self, name):
        # Ausgabe des Trackers direkt an den Browser streamen (115200 Baud ≈ 100 Zeilen/s)
        self.send_response(200)
        self.send_header("Content-Type", "text/csv; charset=utf-8")
        self.send_header("Content-Disposition", 'attachment; filename="%s.csv"' % name)
        self.end_headers()
        count = [0]

        def on_line(line):
            if line.startswith("date,") or (line[:2] == "20" and "," in line):
                self.wfile.write((line + "\n").encode())
                count[0] += 1
                if count[0] % 500 == 0:
                    events.send("logdump", {"name": name, "lines": count[0]})

        try:
            ok, lines = tracker.command("AT+LOGDUMP=" + name, timeout=5, idle_timeout=15, on_line=on_line)
        except RuntimeError as e:
            self.wfile.write(("# Fehler: %s\n" % e).encode())
            return
        events.send("logdump", {"name": name, "lines": count[0], "done": True, "ok": ok})


def main():
    global tracker
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=8080, help="HTTP-Port (Standard 8080)")
    ap.add_argument("--bind", default="0.0.0.0", help="Adresse (Standard alle)")
    ap.add_argument("--mqtt", default="localhost", help="MQTT-Broker (Standard localhost)")
    ap.add_argument("--mqtt-port", type=int, default=1883)
    ap.add_argument("--tracker", help="serielle Schnittstelle (Standard: automatisch)")
    ap.add_argument("--band-sh", default=DEFAULT_BAND_SH, help="Pfad zu surfcam/band.sh")
    ap.add_argument("--data", default=os.path.expanduser("~/surfcam-data"),
                    help="Ordner für Aufzeichnungen (Standard ~/surfcam-data)")
    ap.add_argument("--pi-gnss", default="/dev/ttyAMA0", help="GNSS des HAT (Standard /dev/ttyAMA0), 'aus' = nicht lesen")
    ap.add_argument("--pi-gnss-rate", type=int, default=1, choices=(1, 2, 5, 10),
                    help="Rate in Hz (Standard 1; mit 10 Hz hing das HAT-Modul im Test)")
    ap.add_argument("--pool-port", type=int, default=8890,
                    help="Surf-Pool: UDP-Port für den SRT-Stream vom iPhone (Standard 8890, 0 = aus)")
    ap.add_argument("--moblin-port", type=int, default=2345,
                    help="Surf-Pool: TCP-Port für die Fernsteuerung der iPhone-App Moblin (Standard 2345, 0 = aus)")
    args = ap.parse_args()

    global rx_log, relative, pi_gnss, pi_log, pool_mode, power_meter
    Handler.band_sh = args.band_sh
    Handler.data_dir = args.data
    rx_log = position.DailyCsv(os.path.join(args.data, "rx"), ["rx_utc"] + RX_FIELDS)
    relative = position.Relative(os.path.join(args.data, "position.json"))
    if args.pi_gnss != "aus":
        pi_log = position.DailyCsv(os.path.join(args.data, "pi_gnss"),
                                   ["date", "utc", "time_ms", "lat", "lon", "alt", "fix", "sats", "hdop"])
        pi_gnss = position.PiGnss(args.pi_gnss, args.pi_gnss_rate, relative.on_pi_fix, pi_log)
        pi_gnss.start()
    power_meter = power.Power()
    power_meter.start()
    if args.pool_port:
        pool_mode = pool.Pool(os.path.join(args.data, "clips"), port=args.pool_port,
                              on_change=lambda info: events.send("pool", info))
        if args.moblin_port:
            pool_mode.phone = moblin.MoblinRemote(os.path.join(args.data, "moblin.json"), port=args.moblin_port,
                                                  on_change=lambda info: events.send("pool", pool_mode.info()))
            pool_mode.phone.start()
        pool_mode.start()
    uplink.MqttSubscriber(args.mqtt, args.mqtt_port, uplink.TOPIC, packets.on_message, packets.on_status).start()
    tracker = Tracker(args.tracker)
    tracker.start()
    threading.Thread(target=status_poller, daemon=True).start()

    httpd = ThreadingHTTPServer((args.bind, args.port), Handler)
    httpd.daemon_threads = True
    print("[http] http://%s:%d/" % (args.bind, args.port), flush=True)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
