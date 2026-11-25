#!/bin/bash
# Remove the Zicox driver for all models (CC4, CC2, CC3, M35, HDT334): queues, Bluetooth agents and files.
set -uo pipefail
if [ "$(id -u)" -ne 0 ]; then
  exec sudo "$0" "$@"
fi

MODELS="CC4 CC2 CC3 M35 HDT334"
CONSOLE_USER=$(stat -f %Su /dev/console)

for m in $MODELS; do
  for q in "Zicox_${m}_USB" "Zicox_${m}_BT" "Zicox_${m}_WiFi"; do
    lpstat -v "$q" >/dev/null 2>&1 && lpadmin -x "$q" && echo "removed queue $q"
  done
  if [ "$m" = CC4 ]; then label=com.zicox.btrelay; else label="com.zicox.btrelay.$m"; fi
  if [ -n "$CONSOLE_USER" ] && [ "$CONSOLE_USER" != root ]; then
    launchctl bootout "gui/$(id -u "$CONSOLE_USER")/$label" 2>/dev/null
  fi
  rm -f "/Library/LaunchAgents/$label.plist" "/Library/Printers/PPDs/Contents/Resources/Zicox_$m.ppd"
done

launchctl bootout system/com.zicox.btrelay 2>/dev/null
rm -f /Library/LaunchDaemons/com.zicox.btrelay.plist
rm -rf /Library/Printers/Zicox
rm -f /usr/local/bin/zicoxctl
echo "Zicox driver removed."
