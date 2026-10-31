#!/bin/sh
# Usage: [SBM2_LOAD=state.mss] port/trace.sh <input script> <out.bin> [logic frames]
# Runs port/trace.lua headless in Mesen (see that file for the output format).
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
IN=$(realpath "$1"); OUT=$(realpath "$2")
export SBM2_LOAD=${SBM2_LOAD:+$(realpath "$SBM2_LOAD")}
cd "$ROOT/tools/mesen2"
SBM2_INPUT="$IN" SBM2_TRACE="$OUT" SBM2_FRAMES="${3:-3000}" \
  ./Mesen --testrunner --timeout=86400 "$ROOT/baserom.sfc" "$ROOT/port/trace.lua" >/dev/null 2>&1
echo "trace.sh: $(wc -l < "$OUT.txt") logic frames" >&2
