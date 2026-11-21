#!/bin/bash
# Flash one bring-up stage and report whether the firmware got back to update mode by itself.
#   tools/bringup.sh <stage>
set -uo pipefail
cd "$(dirname "$0")/.."
S="$1"
make -C sdk APP=bringup DEFS=-DSTAGE=$S >/dev/null 2>&1 || { echo "build failed"; exit 2; }
uv run tools/flash.py flash "sdk/build/bringup_STAGE$S/bringup.cry.bin" 2>&1 | tr '\r' '\n' | grep -E 'wrote|started|refusing|Error|not found|expected' 
uv run python - <<PY
import time
from serial.tools import list_ports
t0 = time.monotonic(); gone = False
while time.monotonic() - t0 < ${2:-12}:
    present = any(p.vid == 0x1397 and p.pid == 0x127E for p in list_ports.comports())
    if not present: gone = True
    if gone and present:
        print(f"STAGE $S: PASS (bootloader back after {time.monotonic() - t0:.1f} s)"); raise SystemExit(0)
    time.sleep(0.1)
print(f"STAGE $S: FAIL (bootloader did not come back; port left the bus: {gone})"); raise SystemExit(1)
PY
