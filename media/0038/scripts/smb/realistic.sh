#!/usr/bin/env bash
# tracing-v2 Task 02 — definitive sweep with the REALISTIC reader.
#
# Realistic reader = how traced actually drains the SMB (RFC-0014): it sleeps and
# is woken EITHER by a periodic flush (--wake-ms) OR by the writer signalling
# "buffer getting full" (--fill-pct, default 50%). It then drains everything in
# one sweep and sleeps again. This replaces the earlier worst-case spinning
# reader (--wake-ms 0) that manufactured lap-churn loss.
#
# Usage: scripts/realistic.sh [bench_binary]
set -euo pipefail
BENCH="${1:-/home/sashwinbalaji/proto/perfetto/.claude/worktrees/task4-ringbuf/out/linux_clang_release/shared_ring_buffer_bench}"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$HERE/results"; mkdir -p "$OUT"
LOG="$OUT/realistic.log"; : > "$LOG"
DUR=2.0; REPS=3
# Realistic reader knobs held across the sweep:
WAKE=1            # 1 ms periodic flush
FILL=50           # wake when buffer is 50% full

log(){ echo "[$(date +%H:%M:%S)] $*" | tee -a "$LOG"; }
hdr(){ "$BENCH" --csv-header; }
run(){ local csv="$1"; shift; local r; for r in $(seq 1 "$REPS"); do
  "$BENCH" --csv --wake-ms "$WAKE" --fill-pct "$FILL" "$@" 2>>"$LOG" >> "$csv"; done; }

log "bench=$BENCH  reader: wake=${WAKE}ms fill=${FILL}%  ($REPS reps x ${DUR}s)"

# R1 — loss vs in-rate (8 writers, 512 KB SMB). Where does loss finally appear?
log "R1 loss vs in-rate (8 writers, 512KB)"
F="$OUT/r1_inrate.csv"; hdr >"$F"
for R in 10 25 50 100 150 200 300 400 500 600 700 800; do
  run "$F" --label "r${R}" --writers 8 --msg-size 64 --rate "$R" --chunks 2048 --duration "$DUR"
done

# R2 — loss vs writer count at realistic rates (does contention loss return? no).
log "R2 loss vs writers (50 and 100 MB/s, 512KB)"
F="$OUT/r2_writers.csv"; hdr >"$F"
for RATE in 50 100; do
  for W in 1 2 4 8 16 32 64 128; do
    run "$F" --label "rate${RATE}_w${W}" --writers "$W" --msg-size 64 --rate "$RATE" --chunks 2048 --duration "$DUR"
  done
done

# R3 — robustness to flush interval WITH the fill trigger (old timer-only cliff
# at wake=3ms should be gone — the fill trigger wakes it before overflow).
log "R3 robustness to wake interval (200 MB/s, 512KB, fill=50)"
F="$OUT/r3_wake.csv"; hdr >"$F"
for WK in 1 2 5 10 20 50 100; do
  for r in $(seq 1 $REPS); do
    "$BENCH" --csv --label "wake${WK}" --wake-ms "$WK" --fill-pct "$FILL" \
      --writers 8 --msg-size 64 --rate 200 --chunks 2048 --duration "$DUR" 2>>"$LOG" >>"$F"
  done
done

# R4 — the actual de-bundling operating points (Task-1 grounded).
#  firehose steady: ~8 writers (CPUs), tens of MB/s; pure-sched burst: a few 100 MB/s.
log "R4 realistic operating points"
F="$OUT/r4_operating.csv"; hdr >"$F"
run "$F" --label "firehose_steady_30" --writers 8 --msg-size 64 --rate 30  --chunks 2048 --duration "$DUR"
run "$F" --label "firehose_steady_50" --writers 8 --msg-size 64 --rate 50  --chunks 2048 --duration "$DUR"
run "$F" --label "atrace_sdk_100"     --writers 8 --msg-size 64 --rate 100 --chunks 2048 --duration "$DUR"
run "$F" --label "sched_burst_300"    --writers 8 --msg-size 64 --rate 300 --chunks 2048 --duration "$DUR"
run "$F" --label "sched_burst_500"    --writers 8 --msg-size 64 --rate 500 --chunks 2048 --duration "$DUR"

log "ALL REALISTIC SWEEPS DONE"; touch "$OUT/_REALISTIC_DONE"
