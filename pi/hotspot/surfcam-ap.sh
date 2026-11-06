#!/bin/bash
# Hotspot (uap0) neben dem WLAN-Client (wlan0) betreiben.
#
# Der WLAN-Chip des Pi 5 kann Client und Hotspot gleichzeitig, aber nur auf demselben Kanal.
# Dieses Skript läuft alle 30 s (surfcam-ap.timer) und
#   1. legt die virtuelle Schnittstelle uap0 an, falls sie fehlt,
#   2. stellt den Hotspot auf den Kanal des Client-WLANs, ohne Client-WLAN auf 2,4 GHz Kanal 6,
#   3. nimmt den Hotspot kurz vom Netz, wenn ein bekanntes WLAN in Reichweite ist, der Client
#      sich aber nicht verbindet (anderer Kanal), damit der Client sich anmelden kann.
set -u

CON=surfcam-hotspot
AP=uap0
STA=wlan0
FALLBACK_CH=6           # ohne Client-WLAN: 2,4 GHz (5 GHz Kanal 36-64 in der EU nur drinnen)
BOUNCE_EVERY=300        # s, höchstens so oft den Hotspot für den Client pausieren
STATE=/run/surfcam-ap.bounce
IW=/usr/sbin/iw

log() { echo "surfcam-ap: $*"; }

if ! nmcli -t -f NAME con show | grep -qx "$CON"; then
  log "Verbindung $CON fehlt (pi/hotspot/install.sh ausführen)"
  exit 1
fi

# ---- 1. virtuelle Schnittstelle
if ! $IW dev "$AP" info >/dev/null 2>&1; then
  log "lege $AP an"
  $IW dev "$STA" interface add "$AP" type __ap || exit 1
  mac=$(cat /sys/class/net/$STA/address)
  # eigene MAC-Adresse: Bit "lokal verwaltet" im ersten Byte und letztes Bit umschalten
  # (nur das erste Bit ergäbe dieselbe Adresse wie die P2P-Schnittstelle des Chips)
  ip link set "$AP" address "$(printf %02x $((0x${mac:0:2} ^ 0x02)))${mac:2:13}$(printf %02x $((0x${mac:15:2} ^ 0x01)))"
  sleep 2
fi

# Internet für Geräte im Hotspot: Docker setzt die FORWARD-Kette auf DROP, NetworkManager
# übernimmt nur NAT. Erlaubt wird nur Hotspot -> Client-WLAN (nicht zu den Docker-Netzen).
IPT=/usr/sbin/iptables
if $IPT -n -L DOCKER-USER >/dev/null 2>&1; then
  $IPT -C DOCKER-USER -i "$AP" -o "$STA" -j ACCEPT 2>/dev/null ||
    $IPT -I DOCKER-USER -i "$AP" -o "$STA" -j ACCEPT
  $IPT -C DOCKER-USER -i "$STA" -o "$AP" -m conntrack --ctstate RELATED,ESTABLISHED -j ACCEPT 2>/dev/null ||
    $IPT -I DOCKER-USER -i "$STA" -o "$AP" -m conntrack --ctstate RELATED,ESTABLISHED -j ACCEPT
fi

sta_freq() { $IW dev "$STA" link 2>/dev/null | awk '/freq:/ { print int($2) }'; }
ap_active() { nmcli -t -f GENERAL.STATE con show "$CON" 2>/dev/null | grep -q activated; }

# ---- 3. bekanntes WLAN in Reichweite, Client aber nicht verbunden?
freq=$(sta_freq)
if [ -z "$freq" ] && ap_active; then
  now=$(date +%s)
  last=$(cat "$STATE" 2>/dev/null || echo 0)
  if [ $((now - last)) -ge $BOUNCE_EVERY ]; then
    known=$(nmcli -t -f NAME,TYPE con show | awk -F: -v c="$CON" '$2 == "802-11-wireless" && $1 != c { print $1 }' |
            while read -r name; do nmcli -g 802-11-wireless.ssid con show "$name"; done)
    visible=$(nmcli -t -f SSID dev wifi list ifname "$STA" 2>/dev/null)
    if [ -n "$known" ] && echo "$visible" | grep -qxF -f <(echo "$known"); then
      echo "$now" > "$STATE"
      log "bekanntes WLAN in Reichweite: Hotspot pausiert, Client verbindet sich"
      nmcli con down "$CON" >/dev/null
      for _ in $(seq 20); do
        sleep 1
        freq=$(sta_freq)
        [ -n "$freq" ] && break
      done
    fi
  fi
fi

# ---- 2. Kanal bestimmen
if [ -n "$freq" ]; then
  if [ "$freq" -lt 3000 ]; then
    band=bg
    ch=$(( freq == 2484 ? 14 : (freq - 2407) / 5 ))
  else
    band=a
    ch=$(( (freq - 5000) / 5 ))
    if [ "$ch" -ge 52 ] && [ "$ch" -le 144 ]; then
      # DFS-Kanäle: ein Hotspot ist dort nicht erlaubt (Radarerkennung)
      ap_active && { log "Client auf DFS-Kanal $ch: Hotspot aus"; nmcli con down "$CON" >/dev/null; }
      exit 0
    fi
  fi
else
  band=bg
  ch=$FALLBACK_CH
fi

# Der Chip nimmt den Hotspot bei einem Kanalwechsel des Clients mit, die Einstellung in
# NetworkManager bleibt aber alt: deshalb beide vergleichen
cur=$($IW dev "$AP" info 2>/dev/null | awk '/channel/ { print $2 }')
conf=$(nmcli -g 802-11-wireless.channel con show "$CON")
if ap_active && [ "$cur" = "$ch" ] && [ "$conf" = "$ch" ]; then
  exit 0
fi
log "Hotspot auf Kanal $ch ($([ $band = a ] && echo 5 || echo 2,4) GHz)"
nmcli con modify "$CON" 802-11-wireless.band "$band" 802-11-wireless.channel "$ch"
nmcli con up "$CON" >/dev/null || log "Hotspot konnte nicht starten"
