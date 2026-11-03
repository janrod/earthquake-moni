#!/bin/bash
# Builds and flashes kmoni_tab5, only ever to the verified Tab5.
set -euo pipefail
cd "$(dirname "$0")/.."
FQBN=m5stack:esp32:m5stack_tab5
[ -f kmoni_tab5/secrets.h ] || { echo "Create kmoni_tab5/secrets.h from secrets.h.example" >&2; exit 1; }
arduino-cli compile -b "$FQBN" --build-path build/fw kmoni_tab5
port=$(tools/tab5_port.sh)
echo "Flashing Tab5 on $port"
arduino-cli upload -b "$FQBN" -p "$port" --input-dir build/fw
