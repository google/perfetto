#!/usr/bin/env bash
# tracing-v2 Task 02 — CLEAN SMB-size × starvation grid (no reboots).
#
# The real boot/unlock runs can't give a clean SMB-size curve: the loss is a
# few-second post-boot burst, and exactly how much of it each run catches swings
# the number ~100×. So here we reproduce the *mechanism* — reader starvation —
# with a CONTROLLABLE, REPEATABLE knob instead of a boot: N busy-loops pinned to
# the reader's cores (0-5) starve it at a fixed level. No reboots → fast, many
# reps → genuinely clean medians of "how much does a bigger SMB help when the
# reader is starved at level N".
#
#   nohup scripts/synth_starve.sh > /tmp/synth_starve.out 2>&1 &
set -uo pipefail
TASK=/home/sashwinbalaji/proto/perfetto/tracing_v2/tasks/task-2-smb-bandwidth
DEV=/data/local/tmp/smb
TS=$(date +%Y%m%d_%H%M%S)
OUT=$TASK/results/starve_$TS
mkdir -p "$OUT"
CSV=$OUT/starve.csv; LOG=$OUT/starve.log
REPS=${REPS:-5}; DUR=${DUR:-12}
SMBS="256 512 1024 2048 4096"
MULTS="5 10"
STARVE="0 4 8 12"          # number of busy-loops pinned to cpus 0-5 (the reader's cores)
MASK=0x3f                   # cpus 0-5

log(){ echo "[$(date +%H:%M:%S)] $*" | tee -a "$LOG"; }

# start/stop N busy-loops on cpus 0-5 (controlled, repeatable reader starvation)
start_load(){ local n=$1 i; for ((i=0;i<n;i++)); do adb shell "taskset $MASK sh -c 'while true; do :; done'" >/dev/null 2>&1 & done; }
stop_load(){ adb shell "pkill -f 'while true'" >/dev/null 2>&1 || true; }

hrun(){ adb shell "echo \$\$ > /dev/cpuset/foreground/tasks 2>/dev/null; exec $DEV/smb_replay_harness \
  --replay $DEV/aot_boot_t1.smbr --multiplier $2 --smb-kb $1 --wake-ms 1 --fill-pct 50 \
  --duration $4 --reader-nice 0 --label $3 --csv" 2>>"$LOG" | tr -d '\r' | grep -E '^[a-z]' | head -1; }

log "=== STARVE grid START REPS=$REPS DUR=${DUR}s out=$OUT ==="
timeout 60 adb wait-for-device 2>/dev/null; adb root >/dev/null 2>&1; timeout 30 adb wait-for-device 2>/dev/null
adb shell "mkdir -p $DEV" 2>>"$LOG"
adb push "$TASK/bin/smb_replay_harness.arm64" "$DEV/smb_replay_harness" >/dev/null 2>&1
adb push "$TASK/replay/aot_boot_t1.smbr" "$DEV/aot_boot_t1.smbr" >/dev/null 2>&1
adb shell "chmod 755 $DEV/smb_replay_harness" 2>>"$LOG"
echo "starve_n,smb_kb_o,mult_o,rep,uptime_s,$(adb shell "$DEV/smb_replay_harness --csv-header" 2>/dev/null | tr -d '\r')" > "$CSV"

for n in $STARVE; do
  log "--- starvation level: $n busy-loops on cpus 0-5 ---"
  stop_load; start_load "$n"; sleep 2
  for mult in $MULTS; do
    for smb in $SMBS; do
      for rep in $(seq 1 "$REPS"); do
        line=$(hrun "$smb" "$mult" "starve${n}_${smb}K_${mult}x_r${rep}" "$DUR")
        [ -n "$line" ] && echo "$n,$smb,$mult,$rep,0,$line" >> "$CSV"
      done
      mean=$(awk -F, -v n="$n" -v s="$smb" -v m="$mult" '$1==n && $2==s && $3==m {t+=$16; c++} END{if(c) printf "%.2f",t/c; else print "?"}' "$CSV" 2>/dev/null)
      log "  starve=$n smb=${smb}K ${mult}x -> mean loss=${mean}%"
    done
  done
  stop_load; sleep 1
done
log "--- charts ---"
python3 "$TASK/scripts/plot_starve.py" "$OUT" >>"$LOG" 2>&1 || log "  chart step failed"
log "=== STARVE DONE: $(($(wc -l < "$CSV")-1)) rows -> $OUT ==="
