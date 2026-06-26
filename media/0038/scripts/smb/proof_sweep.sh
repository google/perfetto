#!/usr/bin/env bash
# tracing-v2 Task 02 — the drain-ceiling PROOF sweep (reboot-free).
#
# Hypothesis to prove: loss happens when in-rate > the reader's drain ceiling;
# starvation LOWERS that ceiling; so the multiplier at which you start losing
# depends on how starved the reader is — NOT on the buffer.
#
# Method: hold SMB fixed, sweep the write multiplier (1x..10x) at three reader
# health levels (nice 0 healthy / 5 mild / 10 starved). Each line should sit at
# ~0 loss until the in-rate crosses that level's drain ceiling, then rise.
#
#   nohup scripts/proof_sweep.sh > /tmp/proof_sweep.out 2>&1 &
set -uo pipefail
TASK=/home/sashwinbalaji/proto/perfetto/tracing_v2/tasks/task-2-smb-bandwidth
DEV=/data/local/tmp/smb
TS=$(date +%Y%m%d_%H%M%S)
OUT=$TASK/results/proof_$TS; mkdir -p "$OUT"
CSV=$OUT/proof.csv; LOG=$OUT/proof.log
REPS=${REPS:-5}; DUR=${DUR:-12}; SMB=${SMB:-512}
MULTS="1 2 3 5 7 10"
NICES="0 5 10"
log(){ echo "[$(date +%H:%M:%S)] $*" | tee -a "$LOG"; }
hrun(){ timeout $((DUR+120)) adb shell "echo \$\$ > /dev/cpuset/foreground/tasks 2>/dev/null; exec $DEV/smb_replay_harness \
  --replay $DEV/aot_boot_t1.smbr --multiplier $1 --smb-kb $SMB --wake-ms 1 --fill-pct 50 \
  --duration $DUR --reader-nice $2 --label $3 --csv" 2>>"$LOG" | tr -d '\r' | grep -E '^[a-z]' | head -1; }

log "=== PROOF sweep START SMB=${SMB}K REPS=$REPS DUR=${DUR}s out=$OUT ==="
timeout 60 adb wait-for-device 2>/dev/null; adb root >/dev/null 2>&1; timeout 30 adb wait-for-device 2>/dev/null
source "$(cd "$(dirname "$0")" && pwd)/_devstate.sh"   # vskin_c, cool_to, dev_clean (TARGET_C env)
dev_clean
adb shell "mkdir -p $DEV" 2>>"$LOG"
adb push "$TASK/bin/smb_replay_harness.arm64" "$DEV/smb_replay_harness" >/dev/null 2>&1
adb push "$TASK/replay/aot_boot_t1.smbr" "$DEV/aot_boot_t1.smbr" >/dev/null 2>&1
adb shell "chmod 755 $DEV/smb_replay_harness" 2>>"$LOG"
echo "reader_nice,mult_o,rep,smb_kb_o,uptime_s,$(adb shell "$DEV/smb_replay_harness --csv-header" 2>/dev/null | tr -d '\r')" > "$CSV"

cool_to   # comparable start vskin (drain ceiling is frequency/thermal-sensitive)
for nice in $NICES; do
  log "--- reader nice = $nice ---"
  for mult in $MULTS; do
    for rep in $(seq 1 "$REPS"); do
      line=$(hrun "$mult" "$nice" "nice${nice}_${mult}x_r${rep}")
      [ -n "$line" ] && echo "$nice,$mult,$rep,$SMB,0,$line" >> "$CSV"
    done
    row=$(awk -F, -v n="$nice" -v m="$mult" '$1==n && $2==m {l+=$16; d+=$18; ir+=$14; c++} END{if(c)printf "loss=%.2f%% in=%.0f drain=%.0f",l/c,ir/c,d/c}' "$CSV")
    log "  nice=$nice ${mult}x -> $row"
  done
done
log "--- charts ---"
python3 "$TASK/scripts/plot_proof.py" "$OUT" >>"$LOG" 2>&1 || log "  chart step failed"
log "=== PROOF DONE: $(($(wc -l < "$CSV")-1)) rows -> $OUT ==="
