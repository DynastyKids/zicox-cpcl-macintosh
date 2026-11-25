#!/bin/bash
# Install the Zicox CPCL CUPS driver and create print queues for one printer model.
#
#   ./install.sh                       CC4: USB queue (if the printer is/was seen on USB) + Bluetooth queue
#   ./install.sh --model CC3           another model: CC4 (default), CC2, CC3, M35, HDT334
#   ./install.sh --wifi 192.168.1.50   also add a Wi-Fi queue (raw TCP 9100)
#   ./install.sh --no-bt               skip the Bluetooth agent and queue
#   ./install.sh --no-usb              skip the USB queue
#   ./install.sh --bt-name NAME        Bluetooth name prefix or address (default: the model name, e.g. CC3)
#
# Run it once per model to support several printers. Only the CC4 has been tested on a real printer.
# Needs administrator rights (re-runs itself with sudo).

set -euo pipefail

if [ "$(id -u)" -ne 0 ]; then
  exec sudo "$0" "$@"
fi

cd "$(dirname "$0")"

MODEL=CC4
DO_USB=1
DO_BT=1
WIFI_IP=""
BT_NAME=""
while [ $# -gt 0 ]; do
  case "$1" in
    --model) MODEL="${2:?--model needs a model name}"; shift ;;
    --no-usb) DO_USB=0 ;;
    --no-bt) DO_BT=0 ;;
    --wifi) WIFI_IP="${2:?--wifi needs an IP address}"; shift ;;
    --bt-name) BT_NAME="${2:?--bt-name needs a name or address}"; shift ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
  shift
done

# Bluetooth agent ports per model: print port / control port (zicoxctl -m MODEL uses the control port).
case "$MODEL" in
  CC4)    BT_PORT=9101 ;;
  CC2)    BT_PORT=9111 ;;
  CC3)    BT_PORT=9121 ;;
  M35)    BT_PORT=9131 ;;
  HDT334) BT_PORT=9141 ;;
  *) echo "unknown model: $MODEL (CC4, CC2, CC3, M35, HDT334)" >&2; exit 2 ;;
esac
CTL_PORT=$((BT_PORT + 1))
BT_NAME="${BT_NAME:-$MODEL}"

for f in build/rastertozicox build/ZicoxBluetooth.app/Contents/MacOS/zicox-btrelay build/zicoxctl "ppd/Zicox_$MODEL.ppd"; do
  [ -f "$f" ] || { echo "missing $f - run 'make' first" >&2; exit 1; }
done

PREFIX=/Library/Printers/Zicox
PPD_DIR=/Library/Printers/PPDs/Contents/Resources
PPD="$PPD_DIR/Zicox_$MODEL.ppd"
APP="$PREFIX/ZicoxBluetooth.app"
# The CC4 keeps the original agent name; other models get their own agent.
if [ "$MODEL" = CC4 ]; then LABEL=com.zicox.btrelay; else LABEL="com.zicox.btrelay.$MODEL"; fi
AGENT="/Library/LaunchAgents/$LABEL.plist"
LOG="/tmp/zicox-btrelay$([ "$MODEL" = CC4 ] || echo "-$MODEL").log"

echo "==> Installing files for Zicox $MODEL"
install -d -o root -g wheel -m 755 "$PREFIX/Filter" "$PPD_DIR" /usr/local/bin
install -o root -g wheel -m 755 build/rastertozicox "$PREFIX/Filter/rastertozicox"
install -o root -g wheel -m 755 build/zicoxctl /usr/local/bin/zicoxctl
install -o root -g wheel -m 644 "ppd/Zicox_$MODEL.ppd" "$PPD"
if ! cmp -s build/ZicoxBluetooth.app/Contents/MacOS/zicox-btrelay "$APP/Contents/MacOS/zicox-btrelay" 2>/dev/null; then
  rm -rf "$APP" "$PREFIX/bin"
  ditto build/ZicoxBluetooth.app "$APP"
  chown -R root:wheel "$APP"
fi
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
  USB_URI=$(lpinfo --include-schemes usb -v 2>/dev/null | awk -v m="$MODEL" '$2 ~ ("^usb://[^/]+/" m "([?]|$)") {print $2; exit}' || true)
  if [ -z "$USB_URI" ]; then
    USB_URI=$(lpstat -v 2>/dev/null | grep -oE "usb://[^/ ]+/$MODEL(\?[^ ]*)?" | head -1 || true)
  fi
  if [ -n "$USB_URI" ]; then
    add_queue "Zicox_${MODEL}_USB" "Zicox $MODEL (USB)" "USB" "$USB_URI"
  else
    echo "==> USB: $MODEL not found. Plug it in, switch it on and re-run ./install.sh --model $MODEL --no-bt"
  fi
fi

if [ "$DO_BT" = 1 ]; then
  echo "==> Bluetooth agent ($LABEL, Bluetooth name '$BT_NAME', ports $BT_PORT/$CTL_PORT)"
  # An earlier version used a LaunchDaemon on /dev/cu.CC4*, which does not connect on current macOS.
  launchctl bootout system/com.zicox.btrelay 2>/dev/null || true
  rm -f /Library/LaunchDaemons/com.zicox.btrelay.plist

  cat > "$AGENT" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key><string>$LABEL</string>
  <key>ProgramArguments</key>
  <array>
    <string>$APP/Contents/MacOS/zicox-btrelay</string>
    <string>-a</string><string>$BT_NAME</string>
    <string>-p</string><string>$BT_PORT</string>
    <string>-P</string><string>$CTL_PORT</string>
    <string>-s</string>
  </array>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><true/>
  <key>ThrottleInterval</key><integer>10</integer>
  <key>LimitLoadToSessionType</key><string>Aqua</string>
  <key>StandardErrorPath</key><string>$LOG</string>
</dict>
</plist>
PLIST
  chown root:wheel "$AGENT"
  chmod 644 "$AGENT"

  CONSOLE_USER=$(stat -f %Su /dev/console)
  if [ -n "$CONSOLE_USER" ] && [ "$CONSOLE_USER" != root ]; then
    CONSOLE_UID=$(id -u "$CONSOLE_USER")
    launchctl bootout "gui/$CONSOLE_UID/$LABEL" 2>/dev/null || true
    launchctl bootstrap "gui/$CONSOLE_UID" "$AGENT"
    echo "    started for $CONSOLE_USER (log: $LOG)."
    echo "    If macOS asks whether 'Zicox CC4 蓝牙打印' may use Bluetooth, click Allow."
  else
    echo "    no user logged in; the agent starts at the next login."
  fi

  # GZIP verified over Bluetooth on the CC4 (about 1/5 of the data).
  add_queue "Zicox_${MODEL}_BT" "Zicox $MODEL (Bluetooth)" "Bluetooth" "socket://127.0.0.1:$BT_PORT" -o zcCompress=On
fi

if [ -n "$WIFI_IP" ]; then
  add_queue "Zicox_${MODEL}_WiFi" "Zicox $MODEL (Wi-Fi)" "Wi-Fi $WIFI_IP" "socket://$WIFI_IP:9100" -o zcCompress=On
fi

echo "==> Done. Queues:"
lpstat -v 2>/dev/null | grep -i zicox || true
if [ "$MODEL" != CC4 ]; then
  echo "Note: Zicox $MODEL support is untested. Check with: zicoxctl -m $MODEL status"
fi
