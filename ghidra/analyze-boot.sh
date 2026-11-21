#!/bin/bash
# Import the dumped bootloader (firmware/bootloader.bin, base 0x08000000) and export decompiled C.
set -euo pipefail
cd "$(dirname "$0")"
GHIDRA="${GHIDRA_HOME:-/opt/homebrew/Cellar/ghidra/12.1.4/libexec}"
export JAVA_HOME="${JAVA_HOME:-/opt/homebrew/opt/openjdk@21/libexec/openjdk.jdk/Contents/Home}"
"$GHIDRA/support/analyzeHeadless" project jt4000 \
  -import "../firmware/bootloader.bin" -overwrite \
  -loader BinaryLoader -loader-baseAddr 0x08000000 \
  -processor "ARM:LE:32:Cortex" -cspec default \
  -scriptPath scripts -preScript JtSetup.java 0x08000000 -postScript JtExport.java "out/boot" \
  -analysisTimeoutPerFile 900
