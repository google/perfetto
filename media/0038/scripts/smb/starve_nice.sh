#!/usr/bin/env bash
# tracing-v2 Task 02 — CLEAN SMB-size × reader-starvation grid (no reboots).
#
# Reliable starvation knob: deprioritise the READER via nice (writers stay nice
# 0). Higher nice = the reader gets a smaller CPU share under contention = more
# starved — a clean, repeatable proxy for the CPU starvation a real boot inflicts
# (the busy-loop approach gets cgroup-throttled, so it didn't work). Sweeps
# SMB × reader-nice with reps, no reboots → clean medians of "how much does a
# bigger buffer help as the reader is starved more".
#
#   nohup scripts/starve_nice.sh > /tmp/starve_nice.out 2>&1 &
set -uo pipefail
TASK=/home/sashwinbalaji/proto/perfetto/tracing_v2/tasks/task-2-smb-bandwidth
DEV=/data/local/tmp/smb
TS=$(date +%Y%m%d_%H%M%S)
OUT=$TASK/results/nice_$TS; mkdir -p "$OUT"
CSV=$OUT/nice.csv; LOG=$OUT/nice.log
REPS=${REPS:-6}; DUR=${DUR:-12}
SMBS="256 512 1024 2048 4096"
MULTS="${MULTS:-10 5}"
NICES="0 5 10 19"           # reader nice: 0=normal ... 19=lowest priority (most starved)
log(){ echo "[$(date +%H:%M:%S)] $*" | tee -a "$LOG"; }
hrun(){ timeout $(( $4 + 120 )) adb shell "echo \$\$ > /dev/cpuset/foreground/tasks 2>/dev/null; exec $DEV/smb_replay_harness \
  --replay $DEV/aot_boot_t1.smbr --multiplier $2 --smb-kb $1 --wake-ms 1 --fill-pct 50 \
  --duration $4 --reader-nice $5 --label $3 --csv" 2>>"$LOG" | tr -d '\r' | grep -E '^[a-z]' | head -1; }

log "=== NICE grid START REPS=$REPS DUR=${DUR}s out=$OUT ==="
timeout 60 adb wait-for-device 2>/dev/null; adb root >/dev/null 2>&1; timeout 30 adb wait-for-device 2>/dev/null
source "$(cd "$(dirname "$0")" && pwd)/_devstate.sh"   # vskin_c, cool_to, dev_clean (TARGET_C env)
dev_clean
adb shell "mkdir -p $DEV" 2>>"$LOG"
adb push "$TASK/bin/smb_replay_harness.arm64" "$DEV/smb_replay_harness" >/dev/null 2>&1
adb push "$TASK/replay/aot_boot_t1.smbr" "$DEV/aot_boot_t1.smbr" >/dev/null 2>&1
adb shell "chmod 755 $DEV/smb_replay_harness" 2>>"$LOG"
echo "reader_nice,smb_kb_o,mult_o,rep,uptime_s,$(adb shell "$DEV/smb_replay_harness --csv-header" 2>/dev/null | tr -d '\r')" > "$CSV"

cool_to   # comparable start vskin (drain ceiling is frequency/thermal-sensitive)
for nice in $NICES; do
  log "--- reader nice = $nice ---"
  for mult in $MULTS; do
    for smb in $SMBS; do
      for rep in $(seq 1 "$REPS"); do
        line=$(hrun "$smb" "$mult" "nice${nice}_${smb}K_${mult}x_r${rep}" "$DUR" "$nice")
        [ -n "$line" ] && echo "$nice,$smb,$mult,$rep,0,$line" >> "$CSV"
      done
      mean=$(awk -F, -v n="$nice" -v s="$smb" -v m="$mult" '$1==n && $2==s && $3==m {t+=$16;c++} END{if(c)printf "%.2f",t/c}' "$CSV" 2>/dev/null)
      log "  nice=$nice smb=${smb}K ${mult}x -> mean loss=${mean}%"
    done
  done
done
log "--- charts ---"
python3 "$TASK/scripts/plot_starve.py" "$OUT" --nice >>"$LOG" 2>&1 || log "  chart step failed"
log "=== NICE grid DONE: $(($(wc -l < "$CSV")-1)) rows -> $OUT ==="
