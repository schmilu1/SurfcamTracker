"""Surf-Pool-Modus: Videostream vom iPhone puffern, auf Knopfdruck die letzten x Sekunden als MP4 speichern.

  iPhone (Larix Broadcaster) --SRT--> ffmpeg (Listener, ohne Neukodierung)
      --> 1-s-Stücke (MPEG-TS) im RAM (/dev/shm)  --Knopf-->  Stücke aneinanderhängen --> MP4

Der Pi kodiert nichts neu (der Pi 5 hat keinen H.264-Encoder), er kopiert nur. Ein Stück endet an
einem Schlüsselbild, deshalb ist ein Clip auf etwa 1 s genau (Schlüsselbild-Abstand in Larix: 1 s).
Das Ende eines Stücks ist die Änderungszeit seiner Datei (= Zeitpunkt des Empfangs).
"""

import glob
import json
import os
import re
import shutil
import subprocess
import threading
import time
from collections import deque
from datetime import datetime

CLIP_RE = re.compile(r"^\d{8}_\d{6}_\d{1,3}s(?:_\d+)?\.mp4$")
CLIP_SECONDS = (10, 15, 20, 30, 45, 60, 90, 120)
KEEP_S = 150          # so lange bleiben Stücke im RAM (längster Clip 120 s + Reserve)
WAIT_AFTER_S = 4.0    # so lange auf das Stück warten, das den Knopfdruck enthält


class Pool(threading.Thread):
    def __init__(self, clip_dir, buf_dir="/dev/shm/surfcam-pool", port=8890, latency_ms=500, on_change=None):
        super().__init__(daemon=True)
        self.clip_dir = clip_dir
        self.buf_dir = buf_dir
        self.port = port
        self.latency_ms = latency_ms
        self.on_change = on_change or (lambda info: None)
        self.lock = threading.Lock()
        self.run_no = 0
        self.proc = None
        self.state = "startet"
        self.connected_since = None
        self.video = ""        # z. B. "hevc 3840x2160 30 fps"
        self.audio = ""
        self.codec = ""
        self.log = deque(maxlen=30)
        self.busy = False
        self.last = None       # Ergebnis des letzten Clips
        self.settings_path = os.path.join(clip_dir, "pool.json")
        self.clip_seconds = 30
        os.makedirs(clip_dir, exist_ok=True)
        try:
            with open(self.settings_path) as f:
                s = json.load(f).get("clip_seconds")
                if s in CLIP_SECONDS:
                    self.clip_seconds = s
        except (OSError, ValueError):
            pass

    # ---------------------------------------------------------------- Empfang

    def run(self):
        if not shutil.which("ffmpeg"):
            self.state = "ffmpeg fehlt (sudo apt install ffmpeg)"
            return
        shutil.rmtree(self.buf_dir, ignore_errors=True)
        os.makedirs(self.buf_dir, exist_ok=True)
        threading.Thread(target=self._housekeeping, daemon=True).start()
        while True:
            self.run_no += 1
            self.video = self.audio = self.codec = ""
            self.connected_since = None
            self.state = "wartet auf Stream"
            url = "srt://0.0.0.0:%d?mode=listener&latency=%d" % (self.port, self.latency_ms * 1000)
            cmd = ["ffmpeg", "-hide_banner", "-nostdin", "-nostats", "-loglevel", "info",
                   "-i", url, "-map", "0:v:0", "-map", "0:a?", "-c", "copy",
                   "-f", "segment", "-segment_time", "1", "-segment_format", "mpegts", "-reset_timestamps", "0",
                   os.path.join(self.buf_dir, "%04d_%%06d.ts" % self.run_no)]
            try:
                self.proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
            except OSError as e:
                self.state = "ffmpeg: %s" % e
                time.sleep(5)
                continue
            self.on_change(self.info())
            for raw in self.proc.stderr:
                self._on_ffmpeg(raw.decode("utf-8", "replace").rstrip())
            self.proc.wait()
            print("[pool] ffmpeg beendet (%s)" % self.proc.returncode, flush=True)
            self.state = "Stream beendet"
            self.on_change(self.info())
            time.sleep(1)

    def _on_ffmpeg(self, line):
        if not line:
            return
        self.log.append(line)
        m = re.search(r"Stream #0:\d+.*?: Video: (\w+).*?, (\d{3,5}x\d{3,5}).*?(?:, ([\d.]+) fps)?", line)
        if m and not self.video:
            fps = re.search(r"([\d.]+) (?:fps|tbr)", line)
            self.codec = m.group(1)
            self.video = "%s %s%s" % (m.group(1), m.group(2), (" %s fps" % fps.group(1)) if fps else "")
            self.connected_since = time.time()
            self.state = "empfängt"
            print("[pool] Stream:", self.video, flush=True)
            self.on_change(self.info())
        m = re.search(r"Stream #0:\d+.*?: Audio: (\w+)", line)
        if m and not self.audio:
            self.audio = m.group(1)
        if re.search(r"error|failed|invalid", line, re.I):
            print("[pool] ffmpeg:", line, flush=True)

    def _segments(self, run=None):
        """Fertige Stücke [(pfad, ende, bytes)], ohne das gerade geschriebene."""
        run = self.run_no if run is None else run
        out = []
        for p in sorted(glob.glob(os.path.join(self.buf_dir, "%04d_*.ts" % run))):
            try:
                st = os.stat(p)
            except OSError:
                continue
            out.append((p, st.st_mtime, st.st_size))
        return out[:-1]

    def _housekeeping(self):
        while True:
            time.sleep(2)
            cutoff = time.time() - KEEP_S
            for p in glob.glob(os.path.join(self.buf_dir, "*.ts")):
                try:
                    if os.path.getmtime(p) < cutoff:
                        os.remove(p)
                except OSError:
                    pass

    # ---------------------------------------------------------------- Clips

    def set_clip_seconds(self, s):
        if s not in CLIP_SECONDS:
            raise ValueError("Länge %s s nicht erlaubt" % s)
        self.clip_seconds = s
        with open(self.settings_path, "w") as f:
            json.dump({"clip_seconds": s}, f)

    def clip(self, seconds=None, source="GUI"):
        """Clip im Hintergrund speichern. False, wenn schon einer läuft oder kein Stream da ist."""
        seconds = seconds or self.clip_seconds
        t_press = time.time()
        with self.lock:
            if self.busy:
                return False, "es wird schon ein Clip gespeichert"
            if not self._segments():
                return False, "kein Video im Puffer (Stream vom iPhone läuft nicht)"
            self.busy = True
        threading.Thread(target=self._make_clip, args=(t_press, seconds, source), daemon=True).start()
        return True, "wird gespeichert"

    def _make_clip(self, t_press, seconds, source):
        try:
            run = self.run_no
            deadline = time.time() + WAIT_AFTER_S
            segs = self._segments(run)
            while not any(e >= t_press for _, e, _ in segs) and time.time() < deadline:
                time.sleep(0.2)
                segs = self._segments(run)
            pick = []
            prev_end = None
            for p, end, _ in segs:
                start = prev_end if prev_end is not None else end - 1
                prev_end = end
                if end > t_press - seconds and start < t_press:
                    pick.append(p)
            if not pick:
                raise RuntimeError("keine passenden Stücke im Puffer")
            name = datetime.fromtimestamp(t_press).strftime("%Y%m%d_%H%M%S") + "_%ds" % seconds
            out = os.path.join(self.clip_dir, name + ".mp4")
            n = 1
            while os.path.exists(out):
                n += 1
                out = os.path.join(self.clip_dir, "%s_%d.mp4" % (name, n))
            cmd = ["ffmpeg", "-hide_banner", "-nostdin", "-loglevel", "error", "-y",
                   "-i", "concat:" + "|".join(pick), "-map", "0", "-c", "copy", "-movflags", "+faststart"]
            if self.codec == "hevc":
                cmd += ["-tag:v", "hvc1"]  # sonst spielen iPhone/Mac das Video nicht ab
            tmp = out + ".part"
            r = subprocess.run(cmd + ["-f", "mp4", tmp], capture_output=True, text=True, timeout=120)
            if r.returncode != 0:
                try:
                    os.remove(tmp)
                except OSError:
                    pass
                raise RuntimeError("ffmpeg: " + (r.stderr.strip().splitlines() or ["Fehler"])[-1])
            os.replace(tmp, out)
            self.last = {"ok": True, "name": os.path.basename(out), "source": source, "t": time.time(),
                         "msg": "%s gespeichert (%d Stücke, %.1f MB)" % (
                             os.path.basename(out), len(pick), os.path.getsize(out) / 1e6)}
        except Exception as e:
            self.last = {"ok": False, "name": None, "source": source, "t": time.time(), "msg": "Clip fehlgeschlagen: %s" % e}
        finally:
            self.busy = False
        print("[pool]", source + ":", self.last["msg"], flush=True)
        self.on_change(self.info())

    def clips(self):
        out = []
        for p in sorted(glob.glob(os.path.join(self.clip_dir, "*.mp4")), reverse=True):
            n = os.path.basename(p)
            if CLIP_RE.match(n):
                out.append({"name": n, "bytes": os.path.getsize(p), "mtime": os.path.getmtime(p)})
        return out

    def clip_path(self, name):
        if not CLIP_RE.match(name):
            return None
        p = os.path.join(self.clip_dir, name)
        return p if os.path.isfile(p) else None

    def delete(self, name):
        p = self.clip_path(name)
        if not p:
            raise ValueError("Clip nicht gefunden")
        os.remove(p)

    # ---------------------------------------------------------------- Status

    def info(self):
        segs = self._segments()
        now = time.time()
        buf = segs[-1][1] - segs[0][1] + 1 if segs else 0
        recent = [s for s in segs if s[1] >= now - 10]
        rate = None
        if len(recent) >= 2:
            span = recent[-1][1] - recent[0][1]
            if span > 0:
                rate = sum(s[2] for s in recent[1:]) * 8 / span / 1e6
        state = self.state
        if state == "empfängt" and segs and now - segs[-1][1] > 4:
            state = "Stream stockt"
        return {"state": state, "video": self.video, "audio": self.audio, "since": self.connected_since,
                "buffer_s": round(buf), "mbps": round(rate, 1) if rate else None, "port": self.port,
                "clip_seconds": self.clip_seconds, "clip_choices": CLIP_SECONDS, "busy": self.busy,
                "last": self.last, "addresses": addresses(), "log": list(self.log)[-8:]}


def addresses():
    """IPv4-Adressen des Pi, über die das iPhone streamen kann (WLAN, Hotspot, USB)."""
    try:
        r = subprocess.run(["ip", "-j", "-4", "addr", "show"], capture_output=True, text=True, timeout=3)
        out = []
        for itf in json.loads(r.stdout or "[]"):
            name = itf.get("ifname", "")
            if name == "lo" or name.startswith(("docker", "br-", "veth")):
                continue
            for a in itf.get("addr_info", []):
                kind = {"wlan0": "WLAN", "uap0": "Hotspot"}.get(name, "USB (iPhone)" if name.startswith(("eth1", "usb", "enx")) else name)
                out.append({"if": name, "kind": kind, "ip": a.get("local")})
        return out
    except (OSError, ValueError, subprocess.SubprocessError):
        return []
