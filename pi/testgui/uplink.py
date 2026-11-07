"""Rover-Pakete über MQTT empfangen und dekodieren (nur Standardbibliothek).

Die ChirpStack Gateway Bridge veröffentlicht jedes empfangene LoRa-Paket als Protobuf
(UplinkFrame) auf eu868/gateway/<id>/event/up. Dieses Modul enthält einen minimalen
MQTT-3.1.1-Client (nur Abonnieren, QoS 0), den Protobuf-Leser und den Rover-Decoder.
"""

import math
import socket
import struct
import threading
import time

TOPIC = "eu868/gateway/+/event/up"
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


def _int32(v):
    v &= 0xFFFFFFFFFFFFFFFF
    return v - (1 << 64) if v >= 1 << 63 else v


def parse_uplink(raw):
    """UplinkFrame -> (phy_payload, Frequenz, RSSI, SNR, Gateway-ID)."""
    f = pb_fields(raw)
    phy = f.get(1, [b""])[0]
    freq = rssi = snr = None
    gw = ""
    if 4 in f:
        freq = pb_fields(f[4][0]).get(1, [None])[0]
    if 5 in f:
        rx = pb_fields(f[5][0])
        if 1 in rx:
            gw = rx[1][0].decode("ascii", "replace")
        if 6 in rx:
            rssi = _int32(rx[6][0])
        if 7 in rx:
            snr = round(struct.unpack("<f", rx[7][0])[0], 1)
    return phy, freq, rssi, snr, gw


# ------------------------------------------------------------------ Rover-Paket

def decode_rover(p):
    """Rover-Paket (Version 1, 2 oder 3) -> dict, sonst None."""
    if len(p) < 2 or p[0] != MHDR or PACKET_SIZES.get(p[1] >> 5) != len(p):
        return None
    w = int.from_bytes(p[3:7], "big")
    batt = w >> 27
    t = w & 0x7FFFFFF
    d = {
        "version": p[1] >> 5,
        "id": p[1] & 0x1F,
        "seq": p[2],
        "battery_mv": 3000 + 40 * batt if batt else None,
        "utc_ms": None if t == 0x7FFFFFF else t,
        "lat": int.from_bytes(p[7:11], "big", signed=True) / 1e7,
        "lon": int.from_bytes(p[11:15], "big", signed=True) / 1e7,
        "speed": p[15] / 10,
        "course": round(p[16] * 360 / 256, 1),
        "fix": p[17] >> 6,
        "sats": p[17] & 0x3F,
        "hdop": p[18] / 10,
        "motion_rms": None,
        "motion_peak": None,
        "tilt": None,
        "button": None,   # Version 3: Tastenzähler 0..3 (kurze Drücke, Surf-Pool)
    }
    if d["version"] >= 3:
        d["button"] = p[21] >> 6
        tilt = p[21] & 0x3F
        if 255 not in (p[19], p[20]) and tilt != 63:
            d["motion_rms"] = round(p[19] * 0.05, 2)
            d["motion_peak"] = round(p[20] * 0.1, 1)
            d["tilt"] = tilt * 3
    elif len(p) >= 22 and 255 not in (p[19], p[20], p[21]):
        d["motion_rms"] = round(p[19] * 0.05, 2)
        d["motion_peak"] = round(p[20] * 0.1, 1)
        d["tilt"] = p[21]
    return d


def distance_m(lat1, lon1, lat2, lon2):
    r = 6371000.0
    p1, p2 = math.radians(lat1), math.radians(lat2)
    a = math.sin((p2 - p1) / 2) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(math.radians(lon2 - lon1) / 2) ** 2
    return 2 * r * math.asin(math.sqrt(a))


# ------------------------------------------------------------------ MQTT (minimal, nur Abonnieren)

def _mqtt_str(s):
    b = s.encode()
    return struct.pack("!H", len(b)) + b


def _remaining_len(n):
    out = b""
    while True:
        b = n % 128
        n //= 128
        out += bytes([b | 0x80 if n else b])
        if not n:
            return out


class MqttSubscriber(threading.Thread):
    """Abonniert ein Topic und ruft on_message(topic, payload) für jede Nachricht auf.
    Verbindet sich bei Fehlern automatisch neu."""

    def __init__(self, host, port, topic, on_message, on_status=None, keepalive=30):
        super().__init__(daemon=True)
        self.host, self.port, self.topic = host, port, topic
        self.on_message = on_message
        self.on_status = on_status or (lambda ok, msg: None)
        self.keepalive = keepalive
        self.connected = False

    def _recv_exact(self, sock, n):
        buf = b""
        while len(buf) < n:
            chunk = sock.recv(n - len(buf))
            if not chunk:
                raise ConnectionError("Verbindung geschlossen")
            buf += chunk
        return buf

    def _read_packet(self, sock):
        hdr = self._recv_exact(sock, 1)[0]
        mult, n = 1, 0
        while True:
            b = self._recv_exact(sock, 1)[0]
            n += (b & 0x7F) * mult
            if not b & 0x80:
                break
            mult *= 128
        return hdr, self._recv_exact(sock, n) if n else b""

    def _session(self):
        sock = socket.create_connection((self.host, self.port), timeout=10)
        try:
            client_id = "surfcam-testgui-%d" % int(time.time() * 1000 % 1e9)
            var = _mqtt_str("MQTT") + bytes([4, 0x02]) + struct.pack("!H", self.keepalive)
            body = var + _mqtt_str(client_id)
            sock.sendall(bytes([0x10]) + _remaining_len(len(body)) + body)
            hdr, data = self._read_packet(sock)
            if hdr >> 4 != 2 or len(data) < 2 or data[1] != 0:
                raise ConnectionError("MQTT CONNACK abgelehnt")
            body = struct.pack("!H", 1) + _mqtt_str(self.topic) + bytes([0])
            sock.sendall(bytes([0x82]) + _remaining_len(len(body)) + body)
            self.connected = True
            self.on_status(True, "verbunden mit %s:%d" % (self.host, self.port))
            sock.settimeout(self.keepalive / 2)
            last_ping = time.time()
            while True:
                try:
                    hdr, data = self._read_packet(sock)
                except socket.timeout:
                    hdr = None
                if hdr is not None and hdr >> 4 == 3:  # PUBLISH (QoS 0)
                    tlen = struct.unpack("!H", data[:2])[0]
                    topic = data[2:2 + tlen].decode("utf-8", "replace")
                    qos = (hdr >> 1) & 3
                    payload = data[2 + tlen + (2 if qos else 0):]
                    try:
                        self.on_message(topic, payload)
                    except Exception as e:  # Fehler im Empfänger dürfen die Verbindung nicht beenden
                        print("[mqtt] Fehler bei der Verarbeitung:", e, flush=True)
                if time.time() - last_ping >= self.keepalive / 2:
                    sock.sendall(b"\xc0\x00")  # PINGREQ
                    last_ping = time.time()
        finally:
            self.connected = False
            sock.close()

    def run(self):
        while True:
            try:
                self._session()
            except Exception as e:
                self.on_status(False, "MQTT %s:%d: %s" % (self.host, self.port, e))
            time.sleep(3)
