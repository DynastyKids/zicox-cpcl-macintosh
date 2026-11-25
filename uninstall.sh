#!/bin/bash
# Remove the Zicox CC4 driver, its queues and the Bluetooth agent.
set -uo pipefail
if [ "$(id -u)" -ne 0 ]; then
  exec sudo "$0" "$@"
fi

for q in Zicox_CC4_USB Zicox_CC4_BT Zicox_CC4_WiFi; do
  lpstat -v "$q" >/dev/null 2>&1 && lpadmin -x "$q" && echo "removed queue $q"
done

CONSOLE_USER=$(stat -f %Su /dev/console)
if [ -n "$CONSOLE_USER" ] && [ "$CONSOLE_USER" != root ]; then
  launchctl bootout "gui/$(id -u "$CONSOLE_USER")/com.zicox.btrelay" 2>/dev/null
fi
launchctl bootout system/com.zicox.btrelay 2>/dev/null
rm -f /Library/LaunchAgents/com.zicox.btrelay.plist /Library/LaunchDaemons/com.zicox.btrelay.plist
rm -rf /Library/Printers/Zicox
rm -f /Library/Printers/PPDs/Contents/Resources/Zicox_CC4.ppd
rm -f /usr/local/bin/zicoxctl
echo "Zicox CC4 driver removed."
