#!/usr/bin/env bash
# tracing-v2 Task 02 — SMB bandwidth tolerance, v0 synthetic sweeps.
#
# Drives shared_ring_buffer_bench across the independent variables and writes
# one CSV per experiment into results/. Each row is one bench run.
#
# Findings this is built to surface (from the smoke tests):
#   - The single reader is the throughput ceiling (~200 MB/s on this box).
#   - Loss appears at LOW occupancy from reader/writer hand-off races, scaling
#     with writer count and reader aggressiveness, NOT just buffer fullness.
#
# Usage: scripts/sweep.sh [bench_binary]
set -euo pipefail

BENCH="${1:-/home/sashwinbalaji/proto/perfetto/.claude/worktrees/task4-ringbuf/out/linux_clang_release/shared_ring_buffer_bench}"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$HERE/results"
mkdir -p "$OUT"
LOG="$OUT/sweep.log"
: > "$LOG"

DUR=2.0   # seconds per run
REPS=3    # repetitions per config (hand-off race is noisy → take the median)

log() { echo "[$(date +%H:%M:%S)] $*" | tee -a "$LOG"; }
hdr() { "$BENCH" --csv-header; }

run() {  # run <csv_file> <args...>  — runs REPS times, one CSV row each
  local csv="$1"; shift
  local r
  for r in $(seq 1 "$REPS"); do
    "$BENCH" --csv "$@" 2>>"$LOG" >> "$csv"
  done
}

log "bench = $BENCH"
"$BENCH" --csv-header >/dev/null 2>&1 || { echo "bench not runnable"; exit 1; }

# ---------------------------------------------------------------------------
# E1: Reader ceiling — flat-out producers, unlimited reader, vary writer count.
#     Headline: max sustained drain rate (MB/s) of the single reader, and how
#     it degrades as writer contention rises.
# ---------------------------------------------------------------------------
log "E1 reader ceiling (flat out, vary writers)"
F="$OUT/e1_reader_ceiling.csv"; hdr > "$F"
for W in 1 2 4 8 16 32 64; do
  run "$F" --label "ceiling_w${W}" --writers "$W" --msg-size 64 \
      --rate 0 --drain 0 --chunks 4096 --duration "$DUR"
done

# ---------------------------------------------------------------------------
# E2: Loss-onset knee — fix reader unlimited + SMB, sweep producer in-rate.
#     Headline: where does loss lift off the floor as in-rate climbs toward the
#     reader ceiling? Run for 1 and 8 writers to separate contention from rate.
# ---------------------------------------------------------------------------
log "E2 loss-onset knee (sweep in-rate)"
F="$OUT/e2_knee.csv"; hdr > "$F"
for W in 1 8; do
  for R in 10 25 50 75 100 125 150 175 200 250 300; do
    run "$F" --label "knee_w${W}_r${R}" --writers "$W" --msg-size 64 \
        --rate "$R" --drain 0 --chunks 2048 --duration "$DUR"
  done
done

# ---------------------------------------------------------------------------
# E3: SMB-size effect — fix in-rate + writers, sweep num_chunks (SMB size).
#     Headline: if loss is contention (not capacity), a bigger SMB should NOT
#     remove it. Tests that directly.
# ---------------------------------------------------------------------------
log "E3 SMB size sweep"
F="$OUT/e3_smb_size.csv"; hdr > "$F"
for C in 256 512 1024 2048 4096 8192 16384 32768; do
  run "$F" --label "smb_c${C}" --writers 8 --msg-size 64 \
      --rate 150 --drain 0 --chunks "$C" --duration "$DUR"
done

# ---------------------------------------------------------------------------
# E4: Writer-count contention — fix AGGREGATE in-rate, sweep writers.
#     Headline: loss as a function of contention at constant bandwidth.
# ---------------------------------------------------------------------------
log "E4 writer-count contention (fixed aggregate rate)"
F="$OUT/e4_writers.csv"; hdr > "$F"
for W in 1 2 4 8 16 32 64 128; do
  run "$F" --label "writers_w${W}" --writers "$W" --msg-size 64 \
      --rate 100 --drain 0 --chunks 2048 --duration "$DUR"
done

# ---------------------------------------------------------------------------
# E5: Reader aggressiveness — fix in-rate + writers, sweep reader drain target.
#     Headline: does a flat-out reader lose MORE than one paced near the in-rate
#     (because aggression causes hand-off races)? drain=0 is flat out.
# ---------------------------------------------------------------------------
log "E5 reader pacing effect"
F="$OUT/e5_reader_pacing.csv"; hdr > "$F"
for D in 0 110 130 150 200 300; do
  run "$F" --label "rpace_d${D}" --writers 8 --msg-size 64 \
      --rate 100 --drain "$D" --chunks 2048 --duration "$DUR"
done

# ---------------------------------------------------------------------------
# E6: Message size / fragmentation — fix in-rate + writers, sweep msg size.
#     252 = exactly one chunk payload; 253 = first fragmentation boundary.
#     Headline: cost of small (sched-like) vs large (atrace-print/fragmented).
# ---------------------------------------------------------------------------
log "E6 message size / fragmentation"
F="$OUT/e6_msg_size.csv"; hdr > "$F"
for S in 16 32 64 128 200 252 253 512 1024 4096; do
  run "$F" --label "msg_s${S}" --writers 8 --msg-size "$S" \
      --rate 150 --drain 0 --chunks 2048 --duration "$DUR"
done

log "ALL SWEEPS DONE"
touch "$OUT/_ALL_DONE"
