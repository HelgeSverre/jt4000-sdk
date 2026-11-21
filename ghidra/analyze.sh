#!/bin/bash
# Import a decrypted JT-4000 image into the Ghidra project and export decompiled C.
#   ghidra/analyze.sh [version]      e.g. ghidra/analyze.sh 115
set -euo pipefail
cd "$(dirname "$0")"
V="${1:-115}"
GHIDRA="${GHIDRA_HOME:-/opt/homebrew/Cellar/ghidra/12.1.4/libexec}"
export JAVA_HOME="${JAVA_HOME:-/opt/homebrew/opt/openjdk@21/libexec/openjdk.jdk/Contents/Home}"
"$GHIDRA/support/analyzeHeadless" project jt4000 \
  -import "../firmware/decrypted/JT-4000_V$V.bin" -overwrite \
  -loader BinaryLoader -loader-baseAddr 0x08008000 \
  -processor "ARM:LE:32:Cortex" -cspec default \
  -scriptPath scripts -preScript JtSetup.java -postScript JtExport.java "out/v$V" \
  -analysisTimeoutPerFile 900
