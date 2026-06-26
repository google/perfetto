#!/usr/bin/env bash
# Task 1 — ftrace de-bundling SIZE analysis over a trace or a DIR of traces.
#
#   ./task1_size.sh <trace.pftrace[.gz] | traces-dir> [outdir]
#
# For each trace it: decompresses (traceconv decompress_packets), runs
# ftrace_expand.py (amplification + 3 scenarios + byte-mix + histogram, as JSON),
# grabs the trace_processor ftrace_event count for validation, and collects all
# JSON into <outdir>/ALL_JSON.txt for paste-back.
#
# Copy BOTH of these into perfetto/tools/ (must sit together):
#   tools/task1_size.sh   tools/ftrace_expand.py
# Run from the perfetto repo root, e.g.:
#   OUT=out/linux_clang_release tools/task1_size.sh \
#       ~/Desktop/tracing_v2/traces  ~/Desktop/tracing_v2/task_1_size
#
# Env: OUT=out/<dir> (build dir; auto-detected if unset).

set -uo pipefail

ARG="${1:-}"
[ -z "$ARG" ] && { echo "usage: $0 <trace.pftrace[.gz] | traces-dir> [outdir]"; exit 1; }
OUTDIR="${2:-./task1_size_out}"
mkdir -p "$OUTDIR"
ALL="$OUTDIR/ALL_JSON.txt"; : > "$ALL"
ERR="$OUTDIR/errors.log";   : > "$ERR"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
EXPAND="$SCRIPT_DIR/ftrace_expand.py"

if [ -z "${OUT:-}" ]; then
  for d in out/linux_clang_release out/mac_release out/*; do
    [ -x "$d/trace_processor_shell" ] && { OUT="$d"; break; }
  done
fi
OUT="${OUT:-out/UNKNOWN}"
TP="$OUT/trace_processor_shell"; TC="$OUT/traceconv"

fail=0
[ -f "$EXPAND" ] || { echo "MISSING ftrace_expand.py next to this script: $EXPAND"; fail=1; }
[ -x "$TP" ]     || { echo "MISSING trace_processor_shell: $TP   (set OUT=your_build_dir, run from repo root)"; fail=1; }
[ -x "$TC" ]     || { echo "MISSING traceconv: $TC   (set OUT=your_build_dir)"; fail=1; }
[ "$fail" = 1 ] && exit 1
echo "build dir : $OUT"
echo "outdir    : $OUTDIR   (paste back: $ALL)"

echo "select count(*) from ftrace_event;" > "$OUTDIR/q_count.sql"

process_one() {  # $1=trace  $2=label
  trace="$1"; label="$2"; safe="$(printf '%s' "$label" | tr '/' '_')"
  echo "[$(date +%H:%M:%S)] $label  ($(du -h "$trace" | cut -f1))"

  case "$trace" in
    *.gz) raw="$OUTDIR/$safe.raw.pftrace"; gunzip -c "$trace" > "$raw" ;;
    *)    raw="$trace" ;;
  esac

  # validation: trace_processor's own ftrace event count (reads compressed fine)
  cnt="$("$TP" -q "$OUTDIR/q_count.sql" "$raw" 2>/dev/null | tr -d '"' | tail -1)"

  # ftrace_expand needs a DECOMPRESSED trace
  dec="$OUTDIR/$safe.dec.pftrace"
  "$TC" decompress_packets "$raw" "$dec" >> "$ERR" 2>&1
  python3 "$EXPAND" "$dec" --label "$label" --json > "$OUTDIR/$safe.json" 2>> "$ERR"

  {
    echo "##### $label    (trace_processor ftrace_event count = $cnt)"
    cat "$OUTDIR/$safe.json"
    echo
  } >> "$ALL"

  # cleanup OUR intermediates only — never the user's input
  rm -f "$dec"
  [ "$raw" = "$OUTDIR/$safe.raw.pftrace" ] && rm -f "$raw"
}

if [ -d "$ARG" ]; then base="$ARG"; else base="$(dirname "$ARG")"; fi
n=0
while IFS= read -r f; do
  rel="${f#"$base"/}"; rel="${rel%.gz}"; rel="${rel%.pftrace}"
  process_one "$f" "$rel"
  n=$((n+1))
done < <(
  if [ -d "$ARG" ]; then
    find "$ARG" -type f \( -name '*.pftrace' -o -name '*.pftrace.gz' \) | sort
  else
    printf '%s\n' "$ARG"
  fi
)

echo
echo "DONE — analyzed $n trace(s)."
echo "Paste back: $ALL    (per-trace JSON also in $OUTDIR/<label>.json; any errors in $ERR)"
