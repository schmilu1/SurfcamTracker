"""Stromverbrauch des Pi 5: misst alle 2 s die Spannungswandler (PMIC) und hält 1 h Verlauf.

Der PMIC des Pi 5 misst Strom und Spannung seiner eigenen Wandler (vcgencmd pmic_read_adc). Das ist der
Verbrauch von CPU, RAM, WLAN und I/O. NICHT enthalten sind die Verluste der Wandler und alles, was
direkt an 5 V hängt (LoRa-HAT, Lüfter, USB-Geräte). Diese Anteile werden geschätzt (EXTRA_*).
"""

import os
import re
import shutil
import subprocess
import threading
import time
from collections import deque

INTERVAL_S = 2
HISTORY_S = 3600
PMIC_EFFICIENCY = 0.85   # Wirkungsgrad der Wandler (grobe Annahme)
EXTRA_HAT_W = 0.40       # SX1302-HAT im Empfang inkl. GNSS-Modul (grobe Annahme)
FAN_MAX_W = 0.80         # Lüfter Active Cooler bei voller Drehzahl (grobe Annahme)

# Wandler -> (Gruppe, Beschreibung)
RAILS = {
    "VDD_CORE": ("CPU", "CPU-Kerne"),
    "0V8_SW": ("I/O", "RP1 (USB, Ethernet, GPIO)"),
    "0V8_AON": ("I/O", "Always-on"),
    "1V1_SYS": ("RAM", "RAM / System 1,1 V"),
    "DDR_VDD2": ("RAM", "RAM VDD2"),
    "DDR_VDDQ": ("RAM", "RAM VDDQ"),
    "1V8_SYS": ("I/O", "System 1,8 V"),
    "3V3_SYS": ("I/O", "System 3,3 V (GPIO, HAT-Logik)"),
    "3V7_WL_SW": ("WLAN", "WLAN / Bluetooth"),
    "3V3_DAC": ("I/O", "Audio-DAC"),
    "3V3_ADC": ("I/O", "ADC"),
    "HDMI": ("I/O", "HDMI"),
}
# Bits von vcgencmd get_throttled
THROTTLE_BITS = {0: "Unterspannung", 1: "Takt begrenzt", 2: "gedrosselt", 3: "Temperaturgrenze"}


def _read(path, default=None):
    try:
        with open(path) as f:
            return f.read().strip()
    except OSError:
        return default


def _hwmon(name, attr):
    for d in sorted(os.listdir("/sys/class/hwmon")) if os.path.isdir("/sys/class/hwmon") else []:
        if _read("/sys/class/hwmon/%s/name" % d) == name:
            v = _read("/sys/class/hwmon/%s/%s" % (d, attr))
            return int(v) if v and v.lstrip("-").isdigit() else None
    return None


class Power(threading.Thread):
    def __init__(self):
        super().__init__(daemon=True)
        self.available = bool(shutil.which("vcgencmd"))
        self.lock = threading.Lock()
        self.history = deque(maxlen=HISTORY_S // INTERVAL_S)  # (t, pi_w, total_w, temp, cpu%)
        self.now = None
        self.error = None
        self.energy_wh = 0.0
        self.since = time.time()
        self._cpu_prev = None
        self._proc_prev = {}

    def run(self):
        if not self.available:
            self.error = "vcgencmd fehlt (kein Raspberry Pi?)"
            return
        last = None
        while True:
            t = time.time()
            try:
                s = self._sample()
                with self.lock:
                    self.now = s
                    self.error = None
                    self.history.append((round(t, 1), s["pi_w"], s["total_w"], s["temp_c"], s["cpu_pct"]))
                    if last:
                        self.energy_wh += s["total_w"] * min(t - last, 10) / 3600
                last = t
            except Exception as e:  # Messung darf den Server nie stören
                self.error = str(e)
            time.sleep(max(0.1, INTERVAL_S - (time.time() - t)))

    # ---------------------------------------------------------------- Messung

    def _vc(self, *args):
        return subprocess.run(["vcgencmd"] + list(args), capture_output=True, text=True, timeout=3).stdout

    def _sample(self):
        amps, volts = {}, {}
        for m in re.finditer(r"(\S+)_([AV]) (?:current|volt)\(\d+\)=([\d.]+)", self._vc("pmic_read_adc")):
            (amps if m.group(2) == "A" else volts)[m.group(1)] = float(m.group(3))
        rails = []
        for name in amps:
            if name in volts:
                grp, desc = RAILS.get(name, ("I/O", name))
                rails.append({"name": name, "group": grp, "desc": desc, "v": round(volts[name], 3),
                              "a": round(amps[name], 4), "w": round(volts[name] * amps[name], 3)})
        if not rails:
            raise RuntimeError("pmic_read_adc liefert keine Werte")
        rails.sort(key=lambda r: -r["w"])
        pi_w = sum(r["w"] for r in rails)
        groups = {}
        for r in rails:
            groups[r["group"]] = round(groups.get(r["group"], 0) + r["w"], 3)

        pwm = _hwmon("pwmfan", "pwm1")
        fan_w = FAN_MAX_W * (pwm or 0) / 255
        total_w = pi_w / PMIC_EFFICIENCY + EXTRA_HAT_W + fan_w

        thr = re.search(r"0x([0-9a-fA-F]+)", self._vc("get_throttled"))
        thr = int(thr.group(1), 16) if thr else 0
        temp = _hwmon("cpu_thermal", "temp1_input")
        freq = re.search(r"=(\d+)", self._vc("measure_clock", "arm"))
        return {
            "t": time.time(), "pi_w": round(pi_w, 2), "total_w": round(total_w, 2),
            "extra": {"Verluste Wandler (%.0f %%)" % (PMIC_EFFICIENCY * 100): round(pi_w / PMIC_EFFICIENCY - pi_w, 2),
                      "LoRa-HAT": EXTRA_HAT_W, "Lüfter": round(fan_w, 2)},
            "groups": groups, "rails": rails,
            "input_v": round(volts.get("EXT5V", 0), 3),
            "temp_c": round(temp / 1000, 1) if temp is not None else None,
            "arm_mhz": round(int(freq.group(1)) / 1e6) if freq else None,
            "fan_rpm": _hwmon("pwmfan", "fan1_input"), "fan_pwm": pwm,
            "cpu_pct": self._cpu(), "procs": self._procs(),
            "throttled": thr,
            "throttle_now": [txt for b, txt in THROTTLE_BITS.items() if thr & (1 << b)],
            "throttle_past": [txt for b, txt in THROTTLE_BITS.items() if thr & (1 << (b + 16))],
        }

    def _cpu(self):
        f = _read("/proc/stat", "").splitlines()[0].split()[1:]
        v = [int(x) for x in f]
        idle, total = v[3] + v[4], sum(v)
        prev, self._cpu_prev = self._cpu_prev, (idle, total)
        if not prev or total == prev[1]:
            return None
        return round(100 * (1 - (idle - prev[0]) / (total - prev[1])), 1)

    def _procs(self):
        """Prozesse mit der meisten CPU-Zeit seit der letzten Messung (Prozent eines Kerns)."""
        hz = os.sysconf("SC_CLK_TCK")
        now = time.time()
        cur, out = {}, []
        for pid in os.listdir("/proc"):
            if not pid.isdigit():
                continue
            st = _read("/proc/%s/stat" % pid)
            if not st:
                continue
            name = st[st.find("(") + 1:st.rfind(")")]
            f = st[st.rfind(")") + 2:].split()
            ticks = int(f[11]) + int(f[12])
            cur[pid] = (now, ticks)
            p = self._proc_prev.get(pid)
            if p and now > p[0]:
                pct = (ticks - p[1]) / hz / (now - p[0]) * 100
                if pct >= 0.5:
                    out.append({"pid": int(pid), "name": name, "cpu": round(pct, 1)})
        self._proc_prev = cur
        out.sort(key=lambda x: -x["cpu"])
        return out[:8]

    # ---------------------------------------------------------------- Status

    def info(self, since=0):
        with self.lock:
            hist = [h for h in self.history if h[0] > since]
            return {"available": self.available, "error": self.error, "now": self.now,
                    "history": hist, "interval_s": INTERVAL_S, "history_s": HISTORY_S,
                    "energy_wh": round(self.energy_wh, 2), "energy_since": self.since,
                    "assumptions": {"efficiency": PMIC_EFFICIENCY, "hat_w": EXTRA_HAT_W, "fan_max_w": FAN_MAX_W}}
