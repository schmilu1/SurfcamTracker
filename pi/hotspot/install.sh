#!/bin/bash
# Hotspot einrichten: sudo ./install.sh <Name> <Passwort (mind. 8 Zeichen)>
# Das Passwort wird nur in der NetworkManager-Verbindung auf dem Pi gespeichert (nur root lesbar).
set -eu
[ "$(id -u)" = 0 ] || { echo "mit sudo ausführen"; exit 1; }
[ $# = 2 ] || { echo "Aufruf: sudo $0 <Name> <Passwort>"; exit 1; }
SSID=$1
PASS=$2
[ ${#PASS} -ge 8 ] || { echo "Passwort: mindestens 8 Zeichen (WPA2)"; exit 1; }
DIR=$(cd "$(dirname "$0")" && pwd)
CON=surfcam-hotspot

install -m 755 "$DIR/surfcam-ap.sh" /usr/local/sbin/surfcam-ap

if nmcli -t -f NAME con show | grep -qx "$CON"; then
  nmcli con delete "$CON" >/dev/null
fi
nmcli con add type wifi ifname uap0 con-name "$CON" autoconnect no ssid "$SSID" \
  802-11-wireless.mode ap 802-11-wireless.band bg 802-11-wireless.channel 6 \
  wifi-sec.key-mgmt wpa-psk wifi-sec.psk "$PASS" wifi-sec.proto rsn \
  wifi-sec.pairwise ccmp wifi-sec.group ccmp \
  ipv4.method shared ipv4.addresses 192.168.4.1/24 ipv6.method disabled >/dev/null

install -m 644 "$DIR/surfcam-ap.service" "$DIR/surfcam-ap.timer" /etc/systemd/system/
systemctl daemon-reload
systemctl enable --now surfcam-ap.timer
systemctl start surfcam-ap.service
echo "Hotspot \"$SSID\" eingerichtet: Pi unter 192.168.4.1"
