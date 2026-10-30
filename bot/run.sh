#!/bin/sh
# Usage: [SBM2_LOAD=state.mss] [SBM2_SAVE=frame:path] bot/run.sh <outdir> <inputfile> <shot frames,...>
# Runs the harness headless; writes PNG/WRAM shots, run.cdl and sheet.png.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$1"
OUT=$(realpath "$1"); IN=$(realpath "$2")
LOAD=${SBM2_LOAD:+$(realpath "$SBM2_LOAD")}
rm -f "$OUT"/f*.png "$OUT"/f*.wram "$OUT"/error.txt
cd "$ROOT/tools/mesen2"
status=0
SBM2_INPUT="$IN" SBM2_SHOTS="$3" SBM2_OUT="$OUT" SBM2_LOAD="$LOAD" timeout 600 \
  ./Mesen --testrunner --timeout=86400 "$ROOT/baserom.sfc" "$ROOT/bot/harness.lua" >/dev/null 2>&1 || status=$?
[ -f "$OUT/error.txt" ] && cat "$OUT/error.txt" >&2
ls "$OUT"/f*.png >/dev/null 2>&1 && python3 -I "$ROOT/tools/sbm2/sheet.py" "$OUT/sheet.png" "$OUT"/f*.png
exit $status
