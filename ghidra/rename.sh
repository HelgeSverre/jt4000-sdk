#!/bin/bash
# Apply ghidra/names-v<ver>.tsv (functions, data) and vars-v<ver>.tsv (parameters, locals)
# to the imported program, then re-export decompiled C.
#   ghidra/rename.sh [version]
set -euo pipefail
cd "$(dirname "$0")"
V="${1:-115}"
GHIDRA="${GHIDRA_HOME:-/opt/homebrew/Cellar/ghidra/12.1.4/libexec}"
export JAVA_HOME="${JAVA_HOME:-/opt/homebrew/opt/openjdk@21/libexec/openjdk.jdk/Contents/Home}"
"$GHIDRA/support/analyzeHeadless" project jt4000 -process "JT-4000_V$V.bin" -noanalysis \
  -scriptPath scripts -postScript JtApplyNames.java "names-v$V.tsv" \
  $([ -f "vars-v$V.tsv" ] && echo -postScript JtRenameVars.java "vars-v$V.tsv") \
  -postScript JtExport.java "out/v$V"
