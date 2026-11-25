#!/bin/bash
# Install the Zicox CC4 CUPS driver and create print queues.
#
#   ./install.sh                       USB queue (if the printer is/was seen on USB) + Bluetooth queue
#   ./install.sh --wifi 192.168.1.50   also add a Wi-Fi queue (raw TCP 9100)
#   ./install.sh --no-bt               skip the Bluetooth agent and queue
#   ./install.sh --no-usb              skip the USB queue
#
# Needs administrator rights (re-runs itself with sudo).

set -euo pipefail

if [ "$(id -u)" -ne 0 ]; then
  exec sudo "$0" "$@"
fi

cd "$(dirname "$0")"

DO_USB=1
DO_BT=1
WIFI_IP=""
while [ $# -gt 0 ]; do
  case "$1" in
    --no-usb) DO_USB=0 ;;
    --no-bt) DO_BT=0 ;;
    --wifi) WIFI_IP="${2:?--wifi needs an IP address}"; shift ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
  shift
done

for f in build/rastertozicox build/ZicoxBluetooth.app/Contents/MacOS/zicox-btrelay build/zicoxctl ppd/Zicox_CC4.ppd; do
  [ -f "$f" ] || { echo "missing $f - run 'make' first" >&2; exit 1; }
done

PREFIX=/Library/Printers/Zicox
PPD_DIR=/Library/Printers/PPDs/Contents/Resources
PPD="$PPD_DIR/Zicox_CC4.ppd"
APP="$PREFIX/ZicoxBluetooth.app"
AGENT=/Library/LaunchAgents/com.zicox.btrelay.plist
BT_PORT=9101
CTL_PORT=9102

echo "==> Installing files"
install -d -o root -g wheel -m 755 "$PREFIX/Filter" "$PPD_DIR" /usr/local/bin
install -o root -g wheel -m 755 build/rastertozicox "$PREFIX/Filter/rastertozicox"
install -o root -g wheel -m 755 build/zicoxctl /usr/local/bin/zicoxctl
install -o root -g wheel -m 644 ppd/Zicox_CC4.ppd "$PPD"
rm -rf "$APP" "$PREFIX/bin"
ditto build/ZicoxBluetooth.app "$APP"
chown -R root:wheel "$APP"
xattr -dr com.apple.quarantine "$PREFIX" /usr/local/bin/zicoxctl "$PPD" 2>/dev/null || true

add_queue() { # name description location uri [extra lpadmin options...]
  echo "==> Queue $1 -> $4"
  local name="$1" desc="$2" loc="$3" uri="$4"
  shift 4
  lpadmin -p "$name" -D "$desc" -L "$loc" -v "$uri" -P "$PPD" -E \
          -o printer-error-policy=retry-job -o printer-is-shared=false \
          -o zcMediaTracking=Continuous "$@"
  cupsenable "$name" 2>/dev/null || true
  cupsaccept "$name" 2>/dev/null || true
}

if [ "$DO_USB" = 1 ]; then
  USB_URI=$(lpinfo --include-schemes usb -v 2>/dev/null | awk '/usb:\/\/Zicox\/CC4/ {print $2; exit}' || true)
  if [ -z "$USB_URI" ]; then
    USB_URI=$(lpstat -v 2>/dev/null | grep -o 'usb://Zicox/CC4[^ ]*' | head -1 || true)
  fi
  if [ -n "$USB_URI" ]; then
    add_queue Zicox_CC4_USB "Zicox CC4 (USB)" "USB" "$USB_URI"
  else
    echo "==> USB: CC4 not found. Plug it in, switch it on and re-run ./install.sh --no-bt"
  fi
fi

if [ "$DO_BT" = 1 ]; then
  echo "==> Bluetooth agent (ZicoxBluetooth.app, LaunchAgent com.zicox.btrelay, ports $BT_PORT/$CTL_PORT)"
  # An earlier version used a LaunchDaemon on /dev/cu.CC4*, which does not connect on current macOS.
  launchctl bootout system/com.zicox.btrelay 2>/dev/null || true
  rm -f /Library/LaunchDaemons/com.zicox.btrelay.plist

  cat > "$AGENT" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key><string>com.zicox.btrelay</string>
  <key>ProgramArguments</key>
  <array>
    <string>$APP/Contents/MacOS/zicox-btrelay</string>
    <string>-p</string><string>$BT_PORT</string>
    <string>-P</string><string>$CTL_PORT</string>
    <string>-s</string>
  </array>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><true/>
  <key>ThrottleInterval</key><integer>10</integer>
  <key>LimitLoadToSessionType</key><string>Aqua</string>
  <key>StandardErrorPath</key><string>/tmp/zicox-btrelay.log</string>
</dict>
</plist>
PLIST
  chown root:wheel "$AGENT"
  chmod 644 "$AGENT"

  CONSOLE_USER=$(stat -f %Su /dev/console)
  if [ -n "$CONSOLE_USER" ] && [ "$CONSOLE_USER" != root ]; then
    CONSOLE_UID=$(id -u "$CONSOLE_USER")
    launchctl bootout "gui/$CONSOLE_UID/com.zicox.btrelay" 2>/dev/null || true
    launchctl bootstrap "gui/$CONSOLE_UID" "$AGENT"
    echo "    started for $CONSOLE_USER."
    echo "    If macOS asks whether 'Zicox CC4 蓝牙打印' may use Bluetooth, click Allow."
  else
    echo "    no user logged in; the agent starts at the next login."
  fi

  # GZIP verified over Bluetooth on the CC4 (about 1/5 of the data).
  add_queue Zicox_CC4_BT "Zicox CC4 (Bluetooth)" "Bluetooth" "socket://127.0.0.1:$BT_PORT" -o zcCompress=On
fi

if [ -n "$WIFI_IP" ]; then
  add_queue Zicox_CC4_WiFi "Zicox CC4 (Wi-Fi)" "Wi-Fi $WIFI_IP" "socket://$WIFI_IP:9100" -o zcCompress=On
fi

echo "==> Done. Queues:"
lpstat -v 2>/dev/null | grep -i zicox || true
echo "The old queue using the generic Zebra driver (if any) was left untouched;"
echo "remove it with: sudo lpadmin -x Zicox_CC4"
