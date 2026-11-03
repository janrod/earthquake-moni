#!/bin/bash
# Prints the serial port of THIS Tab5, or fails. Other M5 devices share its USB VID:PID
# (303A:1001) and port names can move, so the port is resolved from the unit's MAC and
# then confirmed by asking the chip itself.
set -euo pipefail
# The MAC goes in tools/tab5_mac (gitignored), or in TAB5_MAC, as AA:BB:CC:DD:EE:FF.
TAB5_MAC="${TAB5_MAC:-$(cat "$(dirname "$0")/tab5_mac" 2>/dev/null || true)}"
if [ -z "$TAB5_MAC" ]; then
  echo "Put this Tab5's MAC in tools/tab5_mac (esptool read-mac shows it)" >&2
  exit 1
fi

port=$(ioreg -r -c IOUSBHostDevice -l -w0 2>/dev/null | awk -v mac="$TAB5_MAC" '
  /"USB Serial Number" = / { inTab5 = index($0, "\"" mac "\"") > 0 }
  inTab5 && /"IOCalloutDevice" = / { gsub(/.*= "|".*/, ""); print; exit }')
if [ -z "$port" ]; then
  echo "Tab5 ($TAB5_MAC) not found on USB" >&2
  exit 1
fi

info=$(esptool --port "$port" read-mac 2>&1) || { echo "$info" >&2; exit 1; }
lower=$(echo "$TAB5_MAC" | tr 'A-F' 'a-f')
if ! grep -q "ESP32-P4" <<<"$info" || ! grep -qi "$lower" <<<"$info"; then
  echo "Refusing: $port is not ESP32-P4 with MAC $TAB5_MAC" >&2
  echo "$info" >&2
  exit 1
fi
echo "$port"
