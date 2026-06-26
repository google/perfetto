#!/usr/bin/env bash
# tracing-v2 Task 02 — CONTROLLED cold-start sweep (reboot-free, repeatable).
#
# Boot is uncontrollable (the storm is already underway when adb reconnects). A
# cold app start is the opposite: WE trigger it, so the storm lands at the same
# point in every run -> repeatable -> clean medians on a *real* heavy event.
#
# Per run: settle to a consistent cold state (force-stop the app, home, idle) ->
# start the harness -> at +1s `am start` the app (the real binder+sched+IO storm)
# -> the reader fights that genuine contention while we drive the SMB at the
# chosen multiplier. Write load = the validated aot_boot replay (1x = real rate).
#
# Two studies: (M) multiplier 1..7x at 512 KB  (does 5x break it during a real
# cold-start?);  (S) SMB 256K..4M at 5x  (does a bigger buffer help in the storm?).
#
#   nohup scripts/run_cold.sh > /tmp/run_cold.out 2>&1 &
set -uo pipefail
TASK=/home/sashwinbalaji/proto/perfetto/tracing_v2/tasks/task-2-smb-bandwidth
DEV=/data/local/tmp/smb
PKG=com.google.android.apps.maps
TS=$(date +%Y%m%d_%H%M%S)
OUT=$TASK/results/cold_$TS; mkdir -p "$OUT"
CSV=$OUT/cold.csv; LOG=$OUT/cold.log
REPS=${REPS:-8}; DUR=${DUR:-12}
M_MULTS="${M_MULTS:-1 2 3 5 7}"; M_SMB=512
S_SMBS="${S_SMBS-256 512 1024 2048 4096}"; S_MULT=5
log(){ echo "[$(date +%H:%M:%S)] $*" | tee -a "$LOG"; }
source "$(cd "$(dirname "$0")" && pwd)/_devstate.sh"   # vskin_c, cool_to (vskin-based cooldown)
cool(){ cool_to; }   # vskin-based, screen-off, soft cap, never aborts (see _devstate.sh)

cold_run(){  # mult smb label -> echo csv line
  adb shell "am force-stop $PKG; input keyevent KEYCODE_HOME" >/dev/null 2>&1
  sleep 3
  adb shell 'input keyevent KEYCODE_WAKEUP; wm dismiss-keyguard' >/dev/null 2>&1
  sleep 1
  local outf; outf=$(mktemp)
  ( adb shell "echo \$\$ > /dev/cpuset/foreground/tasks 2>/dev/null; exec $DEV/smb_replay_harness \
      --replay $DEV/aot_boot_t1.smbr --multiplier $1 --smb-kb $2 --wake-ms 1 --fill-pct 50 \
      --duration $DUR --reader-nice 0 --label $3 --csv" 2>>"$LOG" | tr -d '\r' | grep -E '^[a-z]' | head -1 > "$outf" ) &
  local hp=$!
  sleep 1
  adb shell "am start -W $PKG" >/dev/null 2>&1 &
  wait "$hp"
  local line; line=$(cat "$outf"); rm -f "$outf"
  adb shell "am force-stop $PKG" >/dev/null 2>&1
  echo "$line"
}

log "=== COLD sweep START app=$PKG REPS=$REPS DUR=${DUR}s out=$OUT ==="
timeout 60 adb wait-for-device 2>/dev/null; adb root >/dev/null 2>&1; timeout 30 adb wait-for-device 2>/dev/null
adb shell "mkdir -p $DEV" 2>>"$LOG"
adb push "$TASK/bin/smb_replay_harness.arm64" "$DEV/smb_replay_harness" >/dev/null 2>&1
adb push "$TASK/replay/aot_boot_t1.smbr" "$DEV/aot_boot_t1.smbr" >/dev/null 2>&1
adb shell "chmod 755 $DEV/smb_replay_harness" 2>>"$LOG"
echo "study,mult_o,smb_kb_o,rep,uptime_s,$(adb shell "$DEV/smb_replay_harness --csv-header" 2>/dev/null | tr -d '\r')" > "$CSV"

# Study M — multiplier curve at 512 KB
log "--- Study M: multiplier 1..7x @ ${M_SMB}K (does 5x break during cold-start?) ---"
for mult in $M_MULTS; do
  for rep in $(seq 1 "$REPS"); do
    line=$(cold_run "$mult" "$M_SMB" "coldM_${mult}x_r${rep}")
    [ -n "$line" ] && echo "M,$mult,$M_SMB,$rep,0,$line" >> "$CSV"
    cool
  done
  mean=$(awk -F, -v m="$mult" '$1=="M" && $2==m {l+=$16;c++} END{if(c)printf "%.2f",l/c}' "$CSV")
  log "  ${mult}x -> mean loss=${mean}%  ($(($(grep -c "^M,$mult," "$CSV"))) reps)"
done
# Study S — SMB sweep at 5x
log "--- Study S: SMB 256K..4M @ ${S_MULT}x (does a bigger buffer help in the storm?) ---"
for smb in $S_SMBS; do
  for rep in $(seq 1 "$REPS"); do
    line=$(cold_run "$S_MULT" "$smb" "coldS_${smb}K_r${rep}")
    [ -n "$line" ] && echo "S,$S_MULT,$smb,$rep,0,$line" >> "$CSV"
    cool
  done
  mean=$(awk -F, -v s="$smb" '$1=="S" && $3==s {l+=$16;c++} END{if(c)printf "%.2f",l/c}' "$CSV")
  log "  ${smb}K -> mean loss=${mean}%"
done
log "--- charts ---"
python3 "$TASK/scripts/plot_cold.py" "$OUT" >>"$LOG" 2>&1 || log "  chart step failed"
adb shell "am force-stop $PKG" >/dev/null 2>&1
log "=== COLD sweep DONE: $(($(wc -l < "$CSV")-1)) rows -> $OUT ==="
