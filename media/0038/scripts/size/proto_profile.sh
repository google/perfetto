#!/usr/bin/env bash
# Proto-profile trace(s): where do the ftrace bytes go, TODAY vs DE-BUNDLED.
#
#   ./proto_profile.sh <trace.pftrace[.gz] | traces-dir> [outdir]
#
# Point it at ONE trace, or at a DIR of traces (recurses into subdirs like
# aot/, aot_boot/, ...). For each trace it: decompresses, profiles the existing
# trace, builds a parseable de-bundled copy (ftrace_expand.py --write-profilable),
# profiles that, and appends a compact before/after table to <outdir>/SUMMARY.txt.
#
# Copy BOTH of these into perfetto/tools/ (they must sit together):
#   tools/proto_profile.sh   tools/ftrace_expand.py
# Then run from the perfetto repo root, e.g.:
#   OUT=out/linux_clang_release tools/proto_profile.sh \
#       ~/Desktop/tracing_v2/traces  ~/Desktop/tracing_v2/task_1_proto_profile
#
# Env: OUT=out/<dir> (build dir; auto-detected if unset). KEEP=1 keeps the big
# intermediate .pftrace files (default: deleted to save disk).

set -uo pipefail

ARG="${1:-}"
[ -z "$ARG" ] && { echo "usage: $0 <trace.pftrace[.gz] | traces-dir> [outdir]"; exit 1; }
OUTDIR="${2:-./proto_profile_out}"
KEEP="${KEEP:-0}"
mkdir -p "$OUTDIR"
SUMMARY="$OUTDIR/SUMMARY.txt"; : > "$SUMMARY"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
EXPAND="$SCRIPT_DIR/ftrace_expand.py"

if [ -z "${OUT:-}" ]; then
  for d in out/linux_clang_release out/mac_release out/*; do
    [ -x "$d/trace_processor_shell" ] && { OUT="$d"; break; }
  done
fi
OUT="${OUT:-out/UNKNOWN}"
TP="$OUT/trace_processor_shell"; TRACECONV="$OUT/traceconv"

# sanity
fail=0
[ -f "$EXPAND" ]    || { echo "MISSING ftrace_expand.py next to this script: $EXPAND"; fail=1; }
[ -x "$TP" ]        || { echo "MISSING trace_processor_shell: $TP   (set OUT=your_build_dir, run from repo root)"; fail=1; }
[ -x "$TRACECONV" ] || { echo "MISSING traceconv: $TRACECONV   (set OUT=your_build_dir)"; fail=1; }
[ "$fail" = 1 ] && exit 1
echo "build dir : $OUT"
echo "expander  : $EXPAND"
echo "outdir    : $OUTDIR   (summary: $SUMMARY)"

# the two proto-profiler queries (written once, reused per trace)
cat > "$OUTDIR/q_fields.sql" <<'SQL'
select replace(replace(path,'TracePacket.#ftrace_events.FtraceEventBundle.',''),
               '.FtraceEvent.','>') as field, round(total_size/1e6,2) as mb
from experimental_proto_content
where path like '%FtraceEventBundle%' and total_size > 300000
order by total_size desc limit 20;
SQL
cat > "$OUTDIR/q_buckets.sql" <<'SQL'
select case
   when path like '%compact_sched%'                              then '1 compact_sched (sched packed, today)'
   when path like '%#print.%'                                    then '2 print.buf (atrace)'
   when path like '%sched_switch%' or path like '%sched_waking%' then '3 sched events (full msg)'
   when path like '%FtraceEvent.#timestamp%'                     then '4 per-event timestamp'
   when path like '%FtraceEvent.#cpu%' or path like '%FtraceEventBundle.#cpu%' then '5 per-event cpu'
   when path like '%FtraceEventBundle' or path like '%FtraceEvent' then '0 message/bundle framing'
   else '6 other ftrace' end as bucket,
   round(sum(total_size)/1e6,2) as mb
from experimental_proto_content where path like '%FtraceEventBundle%'
group by 1 order by 1;
SQL

# trace_processor prints one benign "Pid field not found" per packet — drop that
# (and progress spam) from the per-trace log; keep real errors + health info.
NOISE='Pid field not found|^Loading trace|Trace loaded'
run_query() {  # $1=query  $2=trace  ; clean stdout, noise-filtered stderr -> $CURLOG
  "$TP" --analyze-trace-proto-content -q "$1" "$2" 2> "$OUTDIR/.tperr"
  grep -avE "$NOISE" "$OUTDIR/.tperr" 2>/dev/null >> "$CURLOG" || true
}

process_one() {  # $1=trace  $2=label
  trace="$1"; label="$2"
  td="$OUTDIR/$label"; mkdir -p "$td"
  CURLOG="$td/proto_profile.log"; : > "$CURLOG"
  echo "[$(date +%H:%M:%S)] $label  ($(du -h "$trace" | cut -f1))  -> $td"
  { echo "trace: $trace"; echo "build: $OUT"; } >> "$CURLOG"

  case "$trace" in
    *.gz) raw="$td/raw.pftrace"; gunzip -c "$trace" > "$raw" ;;
    *)    raw="$trace" ;;
  esac

  # EXISTING (today)
  run_query "$OUTDIR/q_buckets.sql" "$raw" > "$td/today_buckets.csv"
  run_query "$OUTDIR/q_fields.sql"  "$raw" > "$td/today_fields.csv"

  # CONVERTED (de-bundled, parseable) — needs decompressed input
  dec="$td/decompressed.pftrace"; conv="$td/debundled.pftrace"
  "$TRACECONV" decompress_packets "$raw" "$dec" >> "$CURLOG" 2>&1
  python3 "$EXPAND" "$dec" --write-profilable "$conv" >> "$CURLOG" 2>&1
  run_query "$OUTDIR/q_buckets.sql" "$conv" > "$td/converted_buckets.csv"
  run_query "$OUTDIR/q_fields.sql"  "$conv" > "$td/converted_fields.csv"

  # compact before/after into the shared SUMMARY
  {
    echo "########## $label ##########"
    echo "--- EXISTING (today) — bucketed MB ---";        cat "$td/today_buckets.csv"
    echo "--- CONVERTED (de-bundled) — bucketed MB ---";  cat "$td/converted_buckets.csv"
    echo
  } >> "$SUMMARY"
  # full per-trace detail (incl. top fields + health) stays in the per-trace log
  { echo "--- today top fields ---";     cat "$td/today_fields.csv"
    echo "--- converted top fields ---"; cat "$td/converted_fields.csv"; } >> "$CURLOG"

  # cleanup OUR intermediates only — NEVER the user's input trace.
  if [ "$KEEP" != 1 ]; then
    [ "$raw" = "$td/raw.pftrace" ] && rm -f "$raw"   # only the gunzipped copy
    rm -f "$dec" "$conv"
  fi
}

# collect traces (single file or recurse a dir)
if [ -d "$ARG" ]; then base="$ARG"; else base="$(dirname "$ARG")"; fi
n=0
while IFS= read -r f; do
  rel="${f#"$base"/}"; rel="${rel%.gz}"; rel="${rel%.pftrace}"
  label="$(printf '%s' "$rel" | tr '/' '_')"
  process_one "$f" "$label"
  n=$((n+1))
done < <(
  if [ -d "$ARG" ]; then
    find "$ARG" -type f \( -name '*.pftrace' -o -name '*.pftrace.gz' \) | sort
  else
    printf '%s\n' "$ARG"
  fi
)

rm -f "$OUTDIR/.tperr"
echo
echo "DONE — profiled $n trace(s)."
echo "Paste back: $SUMMARY  (per-trace detail in $OUTDIR/<label>/proto_profile.log)"
echo
echo "NOTE: the CONVERTED trace wraps each event in a 1-event FtraceEventBundle so"
echo "      trace_processor can parse it; the '0 message/bundle framing' bucket is an"
echo "      artifact of that wrapper (real v2 TracePacket.ftrace_event has none)."
echo "      The per-FIELD numbers (print.buf, comm strings, timestamp, cpu) are faithful."
