#!/bin/bash
# Hotspot vollständig entfernen: sudo ./uninstall.sh
set -u
[ "$(id -u)" = 0 ] || { echo "mit sudo ausführen"; exit 1; }
systemctl disable --now surfcam-ap.timer 2>/dev/null
rm -f /etc/systemd/system/surfcam-ap.service /etc/systemd/system/surfcam-ap.timer /usr/local/sbin/surfcam-ap /run/surfcam-ap.bounce
systemctl daemon-reload
nmcli con down surfcam-hotspot 2>/dev/null
nmcli con delete surfcam-hotspot 2>/dev/null
/usr/sbin/iw dev uap0 del 2>/dev/null
nmcli dev connect wlan0 >/dev/null 2>&1
echo "Hotspot entfernt"
