#!/usr/bin/env bash
# tracing-v2 Task 02 — THOROUGH SMB-size × load sweep at REAL boot / first-unlock.
# v2 over run_sweep.sh: pinned start (at boot_completed), 20s runs that integrate
# the whole storm (kills slice-timing variance), real reps for clean medians,
# full boot + unlock_app + unlock_home coverage, and WORKING trace capture
# (/data/misc/perfetto-traces + --background). No deadline — runs the full list.
#
#   nohup scripts/run_sweep_v2.sh > /tmp/run_sweep_v2.out 2>&1 &
set -uo pipefail

TASK=/home/sashwinbalaji/proto/perfetto/tracing_v2/tasks/task-2-smb-bandwidth
BIN_LOCAL=$TASK/bin/smb_replay_harness.arm64
REPLAY_LOCAL=$TASK/replay/aot_boot_t1.smbr
DEV=/data/local/tmp/smb
TD=/data/misc/perfetto-traces
TS=$(date +%Y%m%d_%H%M%S)
OUT=$TASK/results/sweep2_$TS
mkdir -p "$OUT/traces"
CSV=$OUT/sweep.csv
LOG=$OUT/sweep.log
REPS=${REPS:-4}
DUR=${DUR:-20}                 # integrate the whole boot/unlock storm
COOL_TJ=${COOL_TJ:-40000}
COOL_MAX=${COOL_MAX:-150}
PKG=com.google.android.apps.maps

log(){ echo "[$(date +%H:%M:%S)] $*" | tee -a "$LOG"; }
source "$(cd "$(dirname "$0")" && pwd)/_devstate.sh"   # vskin_c, cool_to (vskin-based cooldown)
adb_back(){ timeout 220 adb wait-for-device 2>/dev/null; adb root >/dev/null 2>&1; timeout 60 adb wait-for-device 2>/dev/null; }

cooldown(){ cool_to; }   # vskin-based, screen-off, soft cap, never aborts (see _devstate.sh)

# Pinned start: run the instant boot_completed flips to 1 (binary is on DE
# storage, ready by then). Consistent ~17-18s uptime → comparable runs.
wait_bootcompleted(){
  local t0=$(date +%s)
  while :; do
    [ "$(adb shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" = "1" ] && \
      adb shell "test -x $DEV/smb_replay_harness && test -e /dev/cpuset/foreground/tasks" 2>/dev/null && return 0
    [ $(( $(date +%s) - t0 )) -ge 150 ] && { log "  WARN boot timeout"; return 1; }
    sleep 1
  done
}

hrun(){  # smb mult label dur
  adb shell "echo \$\$ > /dev/cpuset/foreground/tasks 2>/dev/null; exec $DEV/smb_replay_harness \
    --replay $DEV/aot_boot_t1.smbr --multiplier $2 --smb-kb $1 --wake-ms 1 --fill-pct 50 \
    --duration $4 --reader-nice 0 --label $3 --csv" 2>>"$LOG" | tr -d '\r' | grep -E '^[a-z]' | head -1
}

cap_start(){ adb shell "rm -f $TD/$1.pftrace; perfetto -o $TD/$1.pftrace -t $((DUR+3))s -b 64mb sched freq idle am wm binder_driver --background" >/dev/null 2>&1; }
cap_pull(){  # label -> pull + verify, into OUT/traces
  local lbl=$1 t0=$(date +%s) sz=0
  while [ $(( $(date +%s)-t0 )) -lt 40 ]; do
    sz=$(adb shell "stat -c %s $TD/$lbl.pftrace 2>/dev/null" 2>/dev/null | tr -d '\r'); sz=${sz:-0}
    [ "$sz" -gt 5000 ] 2>/dev/null && break; sleep 2
  done
  if [ "$sz" -gt 5000 ] 2>/dev/null; then
    adb pull "$TD/$lbl.pftrace" "$OUT/traces/$lbl.pftrace" >/dev/null 2>&1 && log "  trace saved ($sz B)" || log "  trace pull FAILED"
  else log "  trace empty/missing"; fi
  adb shell "rm -f $TD/$lbl.pftrace" 2>/dev/null
}

datapoint(){  # scenario smb mult rep [trace]
  local scen=$1 smb=$2 mult=$3 rep=$4 trace=${5:-}
  log "RUN $scen smb=${smb}K mult=${mult}x rep=$rep ${trace:+(+trace)}"
  adb reboot 2>>"$LOG"; sleep 4; adb_back
  wait_bootcompleted || { log "  boot failed -> skip"; cooldown; return 1; }
  local up; up=$(adb shell cat /proc/uptime 2>/dev/null | tr -d '\r' | cut -d' ' -f1)
  case $scen in
    boot) : ;;
    unlock_home) adb shell 'input keyevent KEYCODE_WAKEUP; wm dismiss-keyguard' >/dev/null 2>&1 ;;
    unlock_app)  adb shell 'input keyevent KEYCODE_WAKEUP; wm dismiss-keyguard' >/dev/null 2>&1
                 adb shell "am start -W $PKG >/dev/null 2>&1 &" ;;
  esac
  local lbl="${scen}_${smb}K_${mult}x_r${rep}"
  [ -n "$trace" ] && cap_start "$lbl"
  local line; line=$(hrun "$smb" "$mult" "$lbl" "$DUR")
  if [ -n "$line" ]; then
    echo "$scen,$smb,$mult,$rep,$up,$line" >> "$CSV"
    log "  -> $(echo "$line" | awk -F, '{printf "loss=%s%% in=%s occ=%s%% starved=%s%% (up=%.0fs)",$11,$12,$14,$17,'"$up"'}')"
  else log "  -> NO OUTPUT"; fi
  [ -n "$trace" ] && cap_pull "$lbl"
  [ "$scen" = unlock_app ] && adb shell "am force-stop $PKG" >/dev/null 2>&1
  cooldown
}

# ---------------------------------------------------------------------------
log "=== SWEEP v2 START ts=$TS REPS=$REPS DUR=${DUR}s out=$OUT ==="
adb_back; adb shell "mkdir -p $DEV" 2>>"$LOG"
adb push "$BIN_LOCAL" "$DEV/smb_replay_harness" >/dev/null 2>&1
adb push "$REPLAY_LOCAL" "$DEV/aot_boot_t1.smbr" >/dev/null 2>&1
adb shell "chmod 755 $DEV/smb_replay_harness" 2>>"$LOG"
echo "scenario,smb_kb_o,mult_o,rep,uptime_s,$(adb shell "$DEV/smb_replay_harness --csv-header" 2>/dev/null | tr -d '\r')" > "$CSV"

# Build the ordered config list (most valuable first).
LIST=$(mktemp)
{
  # Study A — SMB sweep at the break multipliers (the core question), reps.
  for mult in 5 3; do
    for scen in boot unlock_app unlock_home; do
      for smb in 256 512 1024 2048 4096; do
        for rep in $(seq 1 "$REPS"); do
          tr=""; { [ "$rep" = 1 ] && [ "$mult" = 5 ] && { [ "$smb" = 256 ] || [ "$smb" = 512 ] || [ "$smb" = 2048 ]; }; } && tr="trace"
          echo "$scen $smb $mult $rep $tr"
        done
      done
    done
  done
  # Study B — break-curve fill at 512K (1x safe, 7x/10x past break), 3 reps.
  for mult in 1 7 10; do
    for scen in boot unlock_app unlock_home; do
      for rep in 1 2 3; do echo "$scen 512 $mult $rep"; done
    done
  done
} > "$LIST"
log "--- $(wc -l < "$LIST") datapoints queued (≈$(( $(wc -l < "$LIST") * 2 ))min) ---"

i=0; total=$(wc -l < "$LIST")
while read -r scen smb mult rep trace <&3; do
  i=$((i+1)); log "[$i/$total]"
  datapoint "$scen" "$smb" "$mult" "$rep" "${trace:-}"
  # incremental charts every 12 datapoints so progress is visible
  [ $((i % 12)) -eq 0 ] && python3 "$TASK/scripts/plot_sweep.py" "$OUT" >/dev/null 2>&1
done 3< "$LIST"
rm -f "$LIST"

log "--- final charts ---"
python3 "$TASK/scripts/plot_sweep.py" "$OUT" >>"$LOG" 2>&1 || log "  chart step failed"
{ echo "sweep2 $TS"; echo "rows: $(($(wc -l < "$CSV")-1))"; echo "REPS=$REPS DUR=${DUR}s pinned-start integrating runs"; echo "traces: $(ls "$OUT/traces" 2>/dev/null | wc -l)"; } > "$OUT/MANIFEST.txt"
log "=== SWEEP v2 DONE: $(($(wc -l < "$CSV")-1)) rows, $(ls "$OUT/traces" 2>/dev/null | wc -l) traces -> $OUT ==="
