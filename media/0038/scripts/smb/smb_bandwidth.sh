#!/usr/bin/env bash
# tracing-v2 Task 02 — SMB bandwidth tolerance, REALISTIC reader.
#
# Uses the batched reader (--wake-ms: drain-all-then-sleep), which mimics how
# traced actually drains the SMB. With this reader the reader-laps-writer rewrite
# churn is negligible, so loss reduces to the genuine CAPACITY question:
#     does the SMB hold roughly one wake-interval of data?
#     i.e. loss onsets when  in_rate * wake_ms  >  SMB_size  (or in_rate > drain).
#
# Scope: SMB BANDWIDTH ONLY. Default config models the ftrace firehose:
#   8 writers (~#CPUs), 64 B sched-like events, 512 KB SMB, 1 ms drain cadence.
#
# Usage: scripts/smb_bandwidth.sh [bench_binary]
set -euo pipefail
BENCH="${1:-/home/sashwinbalaji/proto/perfetto/.claude/worktrees/task4-ringbuf/out/linux_clang_release/shared_ring_buffer_bench}"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$HERE/results"; mkdir -p "$OUT"
LOG="$OUT/smb_bandwidth.log"; : > "$LOG"
DUR=2.0; REPS=3
hdr() { "$BENCH" --csv-header; }
run() { local csv="$1"; shift; for r in $(seq 1 $REPS); do "$BENCH" --csv "$@" 2>>"$LOG" >> "$csv"; done; }
log() { echo "[$(date +%H:%M:%S)] $*" | tee -a "$LOG"; }

# B1: the headline — loss vs in-rate, realistic firehose (8w, 512KB SMB, 1ms wake).
#     Expect ~0 loss until in-rate approaches the drain/capacity ceiling.
log "B1 loss vs in-rate (8 writers, 512KB SMB, 1ms batched reader)"
F="$OUT/b1_inrate.csv"; hdr > "$F"
for R in 10 25 50 100 200 300 400 500 600 800; do
  run "$F" --label "in_r${R}" --writers 8 --msg-size 64 --rate $R --wake-ms 1 --chunks 2048 --duration $DUR
done

# B2: wake-interval tradeoff — fixed 200 MB/s, 512KB SMB, sweep drain cadence.
#     Expect loss onset when in_rate*wake_ms > ~SMB (200MB/s*~2.6ms ~ 512KB).
log "B2 loss vs wake interval (8 writers, 200 MB/s, 512KB SMB)"
F="$OUT/b2_wake.csv"; hdr > "$F"
for W in 0.1 0.5 1 2 3 5 10 20; do
  run "$F" --label "wake_${W}" --writers 8 --msg-size 64 --rate 200 --wake-ms $W --chunks 2048 --duration $DUR
done

# B3: SMB size now HELPS (batched reader) — fixed 200 MB/s + 5ms wake, sweep size.
#     With a realistic reader, bigger SMB buys burst headroom (opposite of spin).
log "B3 loss vs SMB size (8 writers, 200 MB/s, 5ms wake)"
F="$OUT/b3_smb.csv"; hdr > "$F"
for C in 256 512 1024 2048 4096 8192 16384; do
  run "$F" --label "smb_c${C}" --writers 8 --msg-size 64 --rate 200 --wake-ms 5 --chunks $C --duration $DUR
done

# B4: writer count no longer matters — fixed 100 MB/s, 1ms wake, sweep writers.
log "B4 loss vs writer count (100 MB/s, 512KB SMB, 1ms wake)"
F="$OUT/b4_writers.csv"; hdr > "$F"
for W in 1 8 24 64 128; do
  run "$F" --label "w${W}" --writers $W --msg-size 64 --rate 100 --wake-ms 1 --chunks 2048 --duration $DUR
done

log "SMB-BANDWIDTH SWEEPS DONE"; touch "$OUT/_SMB_DONE"
