"""Fernsteuerung der iPhone-App Moblin («Remote control»): der Pi spielt den «Assistant».

Moblin (Rolle «Streamer») verbindet sich per WebSocket mit dem Pi (Standard-Port 2345) und
versucht es endlos neu (höchstens alle 10 s). «Verbinden» im GUI schaltet also nur den Empfang
auf dem Pi ein; das iPhone meldet sich dann von selbst.

Protokoll (Quelle: github.com/eerimoq/moblin, Moblin/RemoteControl/RemoteControl.swift), JSON in
WebSocket-Textrahmen, Swift-Codable-Form {"<fall>": {<werte>}}:
  Pi -> iPhone   {"hello":{"apiVersion":"0.1","authentication":{"challenge":..,"salt":..}}}
  iPhone -> Pi   {"identify":{"streamerId":..,"authentication":<hash>}}
  Pi -> iPhone   {"identified":{"result":{"ok":{}}}}            (oder wrongPassword + schliessen)
  Pi -> iPhone   {"request":{"id":1,"data":{"setZoom":{"x":2.0}}}}
  iPhone -> Pi   {"response":{"id":1,"result":{"ok":{}},"data":..}}, {"event":{"data":{"state":{"data":..}}}},
                 {"preview":{"preview":"<base64 JPEG>"}}, {"ping":{}} -> Pi antwortet {"pong":{}}
  hash = b64(sha256(b64(sha256(passwort + salt)) + challenge))

Nur Python-Standardbibliothek (eigener kleiner WebSocket-Server).
"""

import base64
import hashlib
import json
import os
import secrets
import socket
import struct
import threading
import time
from urllib.parse import quote

WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


def rc_hash(challenge, salt, password):
    h = hashlib.sha256((password + salt).encode()).digest()
    h = hashlib.sha256((base64.b64encode(h).decode() + challenge).encode()).digest()
    return base64.b64encode(h).decode()


class WsConn:
    """Minimaler WebSocket-Server-Endpunkt (RFC 6455): Handshake, Rahmen lesen/schreiben."""

    def __init__(self, sock):
        self.sock = sock
        self.lock = threading.Lock()
        self.buf = b""

    def _recv_exact(self, n):
        while len(self.buf) < n:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("Verbindung geschlossen")
            self.buf += chunk
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def handshake(self):
        while b"\r\n\r\n" not in self.buf:
            chunk = self.sock.recv(4096)
            if not chunk or len(self.buf) > 16384:
                raise ConnectionError("kein WebSocket-Handshake")
            self.buf += chunk
        head, self.buf = self.buf.split(b"\r\n\r\n", 1)
        key = None
        for line in head.decode("latin-1").split("\r\n")[1:]:
            k, _, v = line.partition(":")
            if k.strip().lower() == "sec-websocket-key":
                key = v.strip()
        if not key:
            self.sock.sendall(b"HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n")
            raise ConnectionError("kein Sec-WebSocket-Key")
        accept = base64.b64encode(hashlib.sha1((key + WS_GUID).encode()).digest()).decode()
        self.sock.sendall(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                           "Sec-WebSocket-Accept: %s\r\n\r\n" % accept).encode())

    def send(self, payload, opcode=1):
        if isinstance(payload, str):
            payload = payload.encode()
        n = len(payload)
        if n < 126:
            head = struct.pack("!BB", 0x80 | opcode, n)
        elif n < 65536:
            head = struct.pack("!BBH", 0x80 | opcode, 126, n)
        else:
            head = struct.pack("!BBQ", 0x80 | opcode, 127, n)
        with self.lock:
            self.sock.sendall(head + payload)

    def recv(self):
        """Nächste vollständige Nachricht: (opcode, bytes). Kontrollrahmen werden einzeln geliefert."""
        parts, first_op = [], None
        while True:
            b0, b1 = self._recv_exact(2)
            fin, op = b0 & 0x80, b0 & 0x0F
            n = b1 & 0x7F
            if n == 126:
                n = struct.unpack("!H", self._recv_exact(2))[0]
            elif n == 127:
                n = struct.unpack("!Q", self._recv_exact(8))[0]
            if n > 16 * 1024 * 1024:
                raise ConnectionError("Rahmen zu gross")
            mask = self._recv_exact(4) if b1 & 0x80 else None
            data = self._recv_exact(n)
            if mask:
                data = bytes(c ^ mask[i % 4] for i, c in enumerate(data))
            if op >= 8:  # Kontrollrahmen (close, ping, pong), nie fragmentiert
                return op, data
            if op != 0:
                first_op = op
            parts.append(data)
            if fin:
                return first_op or 1, b"".join(parts)

    def close(self):
        try:
            self.send(b"", 8)
        except OSError:
            pass
        try:
            self.sock.close()
        except OSError:
            pass


class MoblinRemote(threading.Thread):
    def __init__(self, settings_path, port=2345, on_change=None):
        super().__init__(daemon=True)
        self.settings_path = settings_path
        self.port = port
        self.on_change = on_change or (lambda info: None)
        self.lock = threading.Lock()
        self.listen_sock = None
        self.conn = None
        self.peer = None
        self.identified = False
        self.since = None
        self.msg = ""
        self.next_id = 1
        self.pending = {}         # id -> [threading.Event, antwort]
        self.state = {}           # letzter "state"-Event (Zoom, live, Licht, ...)
        self.status = {}          # letzte getStatus-Antwort (Akku, Temperatur, Kamera)
        self.preview = None       # (zeit, bytes)
        self.enabled = False
        self.password = None
        self._load()

    # ---------------------------------------------------------------- Einstellungen

    def _load(self):
        try:
            with open(self.settings_path) as f:
                s = json.load(f)
            self.enabled = bool(s.get("enabled"))
            self.password = s.get("password") or None
        except (OSError, ValueError):
            pass
        if not self.password:
            self.password = secrets.token_urlsafe(9)
            self._save()

    def _save(self):
        tmp = self.settings_path + ".tmp"
        with open(tmp, "w") as f:
            json.dump({"enabled": self.enabled, "password": self.password}, f)
        os.chmod(tmp, 0o600)
        os.replace(tmp, self.settings_path)

    def set_enabled(self, on):
        self.enabled = bool(on)
        self._save()
        if not on:
            self._drop("getrennt")
            with self.lock:
                if self.listen_sock:
                    try:
                        self.listen_sock.close()
                    except OSError:
                        pass
                    self.listen_sock = None
        self.msg = "wartet auf das iPhone" if on else "aus"
        self.on_change(self.info())

    # ---------------------------------------------------------------- Server

    def run(self):
        while True:
            if not self.enabled:
                time.sleep(0.5)
                continue
            try:
                with self.lock:
                    if not self.listen_sock:
                        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                        s.bind(("0.0.0.0", self.port))
                        s.listen(2)
                        s.settimeout(1.0)
                        self.listen_sock = s
                        self.msg = "wartet auf das iPhone"
                        print("[moblin] wartet auf Port %d" % self.port, flush=True)
                    s = self.listen_sock
                sock, addr = s.accept()
            except socket.timeout:
                continue
            except OSError as e:
                if self.enabled:
                    self.msg = "Port %d: %s" % (self.port, e)
                    time.sleep(5)
                continue
            threading.Thread(target=self._serve, args=(sock, addr), daemon=True).start()

    def _drop(self, why):
        with self.lock:
            conn, self.conn = self.conn, None
        if conn:
            conn.close()
        self.identified = False
        self.since = None
        for ev in list(self.pending.values()):
            ev[0].set()
        if why:
            self.msg = why

    def _serve(self, sock, addr):
        sock.settimeout(90)  # iPhone sendet alle 30 s einen Ping
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        ws = WsConn(sock)
        try:
            ws.handshake()
        except (OSError, ConnectionError) as e:
            print("[moblin] Handshake %s: %s" % (addr[0], e), flush=True)
            sock.close()
            return
        self._drop(None)  # nur ein iPhone: eine neue Verbindung ersetzt die alte
        with self.lock:
            self.conn = ws
        self.peer = addr[0]
        # Challenge absichtlich kein gültiges Base64 (33 Zeichen): sonst versucht Moblin zuerst App Attest
        challenge, salt = secrets.token_hex(16) + "x", secrets.token_hex(16)
        self.msg = "%s verbunden, Anmeldung …" % addr[0]
        try:
            ws.send(json.dumps({"hello": {"apiVersion": "0.1",
                                          "authentication": {"challenge": challenge, "salt": salt}}}))
            while True:
                op, data = ws.recv()
                if op == 8:
                    break
                if op == 9:
                    ws.send(data, 10)
                    continue
                if op != 1:
                    continue
                self._on_message(ws, json.loads(data.decode()), challenge, salt)
        except (OSError, ConnectionError, ValueError) as e:
            if self.conn is ws:
                print("[moblin] %s: %s" % (addr[0], e), flush=True)
        finally:
            if self.conn is ws:
                keep = self.msg.startswith("falsches Passwort")  # Moblin versucht es dann nicht mehr
                self._drop(None if keep else ("Verbindung zum iPhone getrennt, wartet …" if self.enabled else "aus"))
                print("[moblin] getrennt", flush=True)
                self.on_change(self.info())
            else:
                ws.close()

    def _on_message(self, ws, m, challenge, salt):
        if "ping" in m:
            ws.send('{"pong":{}}')
        elif "identify" in m:
            if m["identify"].get("authentication") == rc_hash(challenge, salt, self.password):
                ws.send('{"identified":{"result":{"ok":{}}}}')
                self.identified = True
                self.since = time.time()
                self.msg = "verbunden"
                print("[moblin] iPhone %s angemeldet" % self.peer, flush=True)
                threading.Thread(target=self._after_login, daemon=True).start()
            else:
                ws.send('{"identified":{"result":{"wrongPassword":{}}}}')
                self.msg = "falsches Passwort: in Moblin unter Remote control das Passwort vom Pi eintragen"
                print("[moblin] falsches Passwort von %s" % self.peer, flush=True)
                raise ConnectionError("falsches Passwort")
            self.on_change(self.info())
        elif "response" in m:
            r = m["response"]
            ev = self.pending.get(r.get("id"))
            if ev:
                ev[1] = r
                ev[0].set()
        elif "event" in m:
            d = m["event"].get("data", {})
            if "state" in d:
                self.state = d["state"].get("data", {})
                self.on_change(self.info())
            elif "status" in d:
                self.status.update({k: v for k, v in d["status"].items() if v is not None})
        elif "preview" in m:
            try:
                self.preview = (time.time(), base64.b64decode(m["preview"].get("preview", "")))
            except ValueError:
                pass

    def _after_login(self):
        self.request("startPreview")
        while self.identified:
            r = self.request("getStatus")
            if r and "data" in r and r["data"]:
                self.status = r["data"].get("getStatus", {})
            time.sleep(3)

    # ---------------------------------------------------------------- Befehle

    def request(self, name, timeout=5.0, **params):
        """Anfrage an das iPhone, wartet auf die Antwort. None ohne Verbindung oder Antwort."""
        ws = self.conn
        if not ws or not self.identified:
            return None
        with self.lock:
            rid = self.next_id
            self.next_id += 1
        ev = [threading.Event(), None]
        self.pending[rid] = ev
        try:
            ws.send(json.dumps({"request": {"id": rid, "data": {name: params}}}))
            ev[0].wait(timeout)
            return ev[1]
        except OSError:
            return None
        finally:
            self.pending.pop(rid, None)

    ACTIONS = {  # GUI-Aktion -> (Moblin-Anfrage, Parameter, Statusfeld)
        "live": ("setLive", "on", "streaming"),
        "record": ("setRecord", "on", "recording"),
        "mute": ("setMute", "on", "muted"),
        "torch": ("setTorch", "on", "torchOn"),
        "stealth": ("setStealthMode", "on", "stealthMode"),
    }

    def action(self, what, value):
        if what in ("connect", "disconnect"):
            self.set_enabled(what == "connect")
            return True, ""
        if not self.identified:
            return False, "iPhone nicht verbunden"
        if what == "zoom":
            x = max(0.5, min(float(value), 50.0))
            r = self.request("setZoom", x=x)
            if r:
                self.state["zoom"] = x
        elif what == "zoom_preset":
            r = self.request("setZoomPreset", id=str(value))
            if r:
                self.state["zoomPreset"] = str(value)
        elif what in self.ACTIONS:
            req, key, field = self.ACTIONS[what]
            r = self.request(req, **{key: bool(value)})
            if r:
                self.state[field] = bool(value)
        else:
            return False, "unbekannte Aktion"
        if not r:
            return False, "keine Antwort vom iPhone"
        res = r.get("result", {})
        if "ok" not in res:
            return False, "iPhone meldet: %s" % ", ".join(res) or "Fehler"
        self.on_change(self.info())
        return True, ""

    # ---------------------------------------------------------------- Status

    def setup_link(self, ip):
        # "assistant" mitgeben (aus): ältere Moblin-Versionen verlangen das Feld eventuell
        cfg = {"remoteControl": {"password": self.password,
                                 "assistant": {"enabled": False, "port": self.port},
                                 "streamer": {"enabled": True, "url": "ws://%s:%d" % (ip, self.port)}}}
        return "moblin://?" + quote(json.dumps(cfg, separators=(",", ":")), safe="")

    def info(self, addresses=()):
        g = self.status.get("general") or {}
        tl = self.status.get("topLeft") or {}
        st = self.state
        return {
            "enabled": self.enabled, "port": self.port, "connected": self.identified, "peer": self.peer,
            "since": self.since, "msg": self.msg, "password": self.password,
            "zoom": st.get("zoom"), "zoom_presets": st.get("zoomPresets") or [], "zoom_preset": st.get("zoomPreset"),
            "live": st.get("streaming"), "record": st.get("recording"), "mute": st.get("muted"),
            "torch": st.get("torchOn"), "stealth": st.get("stealthMode"),
            "battery": g.get("batteryLevel"), "charging": g.get("batteryCharging", st.get("batteryCharging")),
            "flame": g.get("flame"), "wifi": g.get("wiFiSsid"),
            "camera": (tl.get("camera") or {}).get("message"), "zoom_text": (tl.get("zoom") or {}).get("message"),
            "preview_t": self.preview[0] if self.preview else None,
            "setup": [{"kind": a["kind"], "url": "ws://%s:%d" % (a["ip"], self.port), "link": self.setup_link(a["ip"])}
                      for a in addresses],
        }

    def preview_image(self):
        if not self.preview:
            return None, None
        data = self.preview[1]
        ctype = "image/png" if data[:4] == b"\x89PNG" else "image/jpeg"
        return data, ctype
