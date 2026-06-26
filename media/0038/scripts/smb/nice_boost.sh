#!/usr/bin/env bash
# tracing-v2 Task 02 / Task 09 rung-1 — does nice -20 close the starvation loss?
#
# The cheapest rung of the Task-9 ladder: keep the reader in CFS but give it a
# strong negative nice, and see if it wins the CPU under contention well enough
# to lift the starved drain ceiling and close the loss. NO real-time policy yet.
#
# Method: under a HEAVY "combined" load — whole-device `dex2oat -a -f` (compile ALL
# packages, forced) + an app-storm — started from a COMPARABLE temperature (cool to
# <=TARGET_C with the screen off first, so DVFS/throttle headroom is the same each
# run), compare reader-nice 0 / -10 / -20 at a few write multipliers, interleaved
# rep-by-rep so all three see the same load slice. Idle control confirms nice does
# nothing without contention. Whole harness confined to traced's cpuset 0-5.
#
# Knobs: TARGET_C (start vskin gate, default 37C), COOL_TIMEOUT (default 360s),
#        REPS, DUR, MULTS.   NOTE: `-a -f` is a multi-minute whole-device compile;
#        runs are longer and hotter than the old single-app load. Keep the device
#        on power. We measure trace loss, NOT energy (that's a separate study).
#
#   nohup scripts/nice_boost.sh > /tmp/nice_boost.out 2>&1 &
set -uo pipefail
TASK=/home/sashwinbalaji/proto/perfetto/tracing_v2/tasks/task-2-smb-bandwidth
DEV=/data/local/tmp/smb
CPUSET=/dev/cpuset/foreground/tasks
TS=$(date +%Y%m%d_%H%M%S)
OUT=$TASK/results/niceboost_$TS; mkdir -p "$OUT"
CSV=$OUT/nice_boost.csv; LOG=$OUT/nice_boost.log
REPS=${REPS:-5}; DUR=${DUR:-12}
MULTS="${MULTS:-3 5 7 10}"
IDLE_MULTS="${IDLE_MULTS:-5 10}"
NICES="0 -10 -20"
SMB=512
log(){ echo "[$(date +%H:%M:%S)] $*" | tee -a "$LOG"; }
source "$(cd "$(dirname "$0")" && pwd)/_devstate.sh"   # vskin_c, cool_to, dev_clean (TARGET_C/COOL_TIMEOUT env)

# run one harness measurement: $1=nice $2=mult $3=label  -> one csv data line
hrun(){ local out; out=$(timeout $((DUR+120)) adb shell "echo \$\$ > $CPUSET 2>/dev/null; exec $DEV/smb_replay_harness \
  --replay $DEV/aot_boot_t1.smbr --multiplier $2 --smb-kb $SMB --wake-ms 1 --fill-pct 50 \
  --duration $DUR --reader-nice $1 --label $3 --csv" 2>>"$LOG" | tr -d '\r' | grep -E '^[a-z]' | head -1)
  [ -z "$out" ] && log "  [warn] no row for '$3' (timeout/empty) — continuing"
  echo "$out"; }

PKG=$(adb shell pm list packages 2>/dev/null | grep -qE 'package:com.google.android.apps.maps$' \
      && echo com.google.android.apps.maps || echo com.android.settings)
load_start(){  # sustained WHOLE-DEVICE dex2oat (-a -f) loop + bursty app-storm loop
  adb shell "nohup sh -c 'while true; do cmd package compile -m speed -a -f >/dev/null 2>&1; done' >/dev/null 2>&1 &"
  adb shell "nohup sh -c 'while true; do am start -W $PKG >/dev/null 2>&1; sleep 0.4; am force-stop $PKG >/dev/null 2>&1; done' >/dev/null 2>&1 &"
}
load_stop(){ adb shell "pkill -f 'cmd package compile' 2>/dev/null; pkill -f dex2oat 2>/dev/null; pkill -f 'am start -W' 2>/dev/null; pkill -f 'while true' 2>/dev/null; am force-stop $PKG 2>/dev/null" >/dev/null 2>&1 || true; }
trap load_stop EXIT INT TERM

med(){ awk -F, -v n="$1" -v m="$2" -v ld="$3" -v c="$4" '$1==n&&$2==m&&$4==ld{v[c++]=$col} ' col="$5" "$CSV" >/dev/null 2>&1; }
# median helper (sort the chosen column for matching rows)
medcol(){ # $1=nice $2=mult $3=load $4=csvcol
  awk -F, -v n="$1" -v m="$2" -v ld="$3" -v c="$4" '$1==n&&$2==m&&$4==ld{print $c}' "$CSV" \
    | sort -n | awk '{a[NR]=$1} END{if(NR){if(NR%2)print a[(NR+1)/2]; else printf "%.3f",(a[NR/2]+a[NR/2+1])/2}}'; }

log "=== nice-boost (rung 1) START reps=$REPS dur=${DUR}s mults='$MULTS' load-app=$PKG out=$OUT ==="
timeout 60 adb wait-for-device 2>/dev/null; adb root >/dev/null 2>&1; timeout 30 adb wait-for-device 2>/dev/null
dev_clean                                  # start clean: kill any orphaned load/harness
adb shell "mkdir -p $DEV" 2>>"$LOG"
adb push "$TASK/bin/smb_replay_harness.arm64" "$DEV/smb_replay_harness" >/dev/null 2>&1
adb push "$TASK/replay/aot_boot_t1.smbr" "$DEV/aot_boot_t1.smbr" >/dev/null 2>&1
adb shell "chmod 755 $DEV/smb_replay_harness" 2>>"$LOG"
echo "reader_nice,mult,rep,load,$(adb shell "$DEV/smb_replay_harness --csv-header" 2>/dev/null | tr -d '\r')" > "$CSV"
# CSV cols after 4 prepended: loss_rate_pct=15, in_rate=16, drain=17, reader_wait_pct=21

# ---- idle control (no load): nice should NOT change loss ----
log "--- IDLE control ---"
for mult in $IDLE_MULTS; do
  for rep in $(seq 1 3); do
    for nice in $NICES; do
      line=$(hrun "$nice" "$mult" "idle_n${nice}_${mult}x_r${rep}")
      [ -n "$line" ] && echo "$nice,$mult,$rep,idle,$line" >> "$CSV"
    done
  done
  l0=$(medcol 0 "$mult" idle 15); l20=$(medcol -20 "$mult" idle 15)
  log "  idle ${mult}x  loss: nice0=${l0}%  nice-20=${l20}%"
done

# ---- combined load: the real contention ----
log "--- COMBINED load (dex2oat -a -f + app-storm) ---"
cool_to                                   # comparable starting temperature
load_start; log "  load started (start temp ${START_TEMP}C), settling 12s..."; sleep 12
for mult in $MULTS; do
  for rep in $(seq 1 "$REPS"); do
    for nice in $NICES; do            # interleave nice0 / nice-20 within each rep
      line=$(hrun "$nice" "$mult" "comb_n${nice}_${mult}x_r${rep}")
      [ -n "$line" ] && echo "$nice,$mult,$rep,combined,$line" >> "$CSV"
    done
  done
  l0=$(medcol 0 "$mult" combined 15);  l20=$(medcol -20 "$mult" combined 15)
  d0=$(medcol 0 "$mult" combined 17);  d20=$(medcol -20 "$mult" combined 17)
  w0=$(medcol 0 "$mult" combined 21);  w20=$(medcol -20 "$mult" combined 21)
  log "  comb ${mult}x  loss: nice0=${l0}% -> nice-20=${l20}%   drain: ${d0}->${d20} MB/s   wait%: ${w0}->${w20}"
done
load_stop
log "  start vskin was ${START_TEMP:-?}C, end vskin $(vskin_c)C"
log "=== nice-boost DONE: $(($(wc -l < "$CSV")-1)) rows -> $OUT ==="
