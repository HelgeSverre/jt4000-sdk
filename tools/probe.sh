#!/bin/bash
# Run start-up probes 0..N in order while the user holds OSC + SETTINGS.
set -uo pipefail
cd "$(dirname "$0")/.."
wait_port() {  # $1 = seconds, $2 = "fresh" to require the port to vanish first
uv run python - <<PY
import time, sys
from serial.tools import list_ports
t0 = time.monotonic(); gone = "$2" != "fresh"
while time.monotonic() - t0 < $1:
    present = any(p.vid == 0x1397 and p.pid == 0x127E for p in list_ports.comports())
    if not present: gone = True
    if gone and present: sys.exit(0)
    time.sleep(0.05)
sys.exit(1)
PY
}
wait_port 180 any || { echo "synth never appeared in update mode"; exit 2; }
echo "bootloader present, starting probes"
for item in "$@"; do
  make -C sdk APP=bringup DEFS="$item" >/dev/null 2>&1 || { echo "$item: build failed"; exit 2; }
  tag="bringup$(echo "$item" | sed 's/-D/_/; s/=//')"
  out=$(uv run tools/flash.py flash "sdk/build/$tag/bringup.cry.bin" 2>&1 | tr '\r' '\n' | grep -E 'started|refusing|expected|Error' | head -1)
  if wait_port 8 fresh; then echo "$item: PASS"; else echo "$item: FAIL ($out)"; exit 1; fi
  sleep 0.5
done
echo "all probes passed"
