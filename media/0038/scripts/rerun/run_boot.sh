#!/usr/bin/env bash
# RFC rerun — FAITHFUL (duplication) session sweeps, REBOOT conditions.
#
# Covers RFC 1a / 1b(N=10) / 1d for the two conditions that need a real reboot:
#   real_boot     — harness started at boot_completed (the boot storm)
#   first_unlock  — same reboot: after boot, wake + dismiss keyguard + cold-launch
#                   the app, harness run again (the first-unlock storm)
# Both are CO-CAPTURED per reboot (2 datapoints / reboot) to halve reboot count.
# Model is DUPLICATION (`--dup-mult N`). 3 reps (boot is noisy + slow). Rows append
# to the same shared raw CSV as run_nonreboot.sh.
#
# Robust: no `set -e`; every adb timeout-wrapped; a failed boot/datapoint is logged
# and skipped; cooldown is a soft cap; the FD-3 loop keeps adb from eating the list.
#
#   nohup rfc_rerun/scripts/run_boot.sh > /tmp/rfc_boot.out 2>&1 &
set -uo pipefail
TASK=/home/sashwinbalaji/proto/perfetto/tracing_v2/tasks/task-2-smb-bandwidth
RFC=$TASK/rfc_rerun
DEV=/data/local/tmp/smb
PKG=com.google.android.apps.maps
TS=$(date +%Y%m%d_%H%M%S)
OUT=$RFC/results/boot_$TS; mkdir -p "$OUT"
RAW=$RFC/results/rfc_raw.csv
LOG=$OUT/run.log
REPS=${REPS:-3}; DUR=${DUR:-20}        # integrate the whole storm
A_N="${A_N:-1 2 3 5 7 10 14}"
B_N="${B_N:-10}"; B_SMB="${B_SMB:-256 512 1024 2048 4096}"
D_N="${D_N:-3 5 7 10}"; D_NICE="${D_NICE:-0 -10}"

log(){ echo "[$(date +%H:%M:%S)] $*" | tee -a "$LOG"; }
source "$TASK/scripts/_devstate.sh"    # vskin_c, cool_to, dev_clean
adb_back(){ timeout 220 adb wait-for-device 2>/dev/null; adb root >/dev/null 2>&1; timeout 60 adb wait-for-device 2>/dev/null; }

emit(){ [ -n "$7" ] && echo "$1,$2,$3,$4,$5,$6,$7" >> "$RAW"; }

# Wait until boot_completed AND the binary+cpuset are ready (binary is on DE
# storage, present pre-unlock). Returns 1 on timeout (caller skips + cools).
wait_bootcompleted(){
  local t0; t0=$(date +%s)
  while :; do
    [ "$(timeout 10 adb shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" = "1" ] && \
      timeout 10 adb shell "test -x $DEV/smb_replay_harness && test -e /dev/cpuset/foreground/tasks" 2>/dev/null && return 0
    [ $(( $(date +%s)-t0 )) -ge 150 ] && { log "  WARN boot timeout"; return 1; }
    sleep 1
  done
}

# One duplication harness run, echoes the harness csv line. args: N smb nice label
hrun_dup(){
  timeout $((DUR+120)) adb shell "echo \$\$ > /dev/cpuset/foreground/tasks 2>/dev/null; exec $DEV/smb_replay_harness \
    --replay $DEV/aot_boot_t1.smbr --multiplier 1 --dup-mult $1 --smb-kb $2 --wake-ms 1 --fill-pct 50 \
    --duration $DUR --reader-nice $3 --label $4 --csv" 2>>"$LOG" | tr -d '\r' | grep ',' | tail -1
}

# One reboot → capture BOTH real_boot and first_unlock at (sweep,N,smb,nice,rep).
reboot_capture(){  # sweep N smb nice rep
  local sweep=$1 N=$2 smb=$3 nice=$4 rep=$5
  log "REBOOT $sweep N=$N smb=${smb}K nice=$nice rep=$rep"
  timeout 60 adb reboot 2>>"$LOG"; sleep 4; adb_back
  wait_bootcompleted || { log "  boot failed -> skip"; cool_to; return 1; }
  # --- real_boot: run immediately during the boot storm ---
  local lb; lb=$(hrun_dup "$N" "$smb" "$nice" "${sweep}_real_boot_n${N}_${smb}K_y${nice}_r${rep}")
  if [ -n "$lb" ]; then emit "$sweep" real_boot "$N" "$smb" "$nice" "$rep" "$lb"
       log "  real_boot   -> loss=$(echo "$lb"|cut -d, -f11)% occ=$(echo "$lb"|cut -d, -f14)% starved=$(echo "$lb"|cut -d, -f17)%"
  else log "  real_boot   -> NO OUTPUT"; fi
  # --- first_unlock: wake, dismiss keyguard, cold-launch app, run again ---
  timeout 15 adb shell 'input keyevent KEYCODE_WAKEUP; wm dismiss-keyguard' >/dev/null 2>&1
  timeout $((DUR+30)) adb shell "am start -W $PKG >/dev/null 2>&1 &" >/dev/null 2>&1
  local lu; lu=$(hrun_dup "$N" "$smb" "$nice" "${sweep}_first_unlock_n${N}_${smb}K_y${nice}_r${rep}")
  if [ -n "$lu" ]; then emit "$sweep" first_unlock "$N" "$smb" "$nice" "$rep" "$lu"
       log "  first_unlock-> loss=$(echo "$lu"|cut -d, -f11)% occ=$(echo "$lu"|cut -d, -f14)% starved=$(echo "$lu"|cut -d, -f17)%"
  else log "  first_unlock-> NO OUTPUT"; fi
  timeout 15 adb shell "am force-stop $PKG" >/dev/null 2>&1
  cool_to
}

# ---------------------------------------------------------------------------
log "=== RFC boot START ts=$TS reps=$REPS dur=${DUR}s out=$OUT ==="
adb_back
timeout 20 adb shell "mkdir -p $DEV" 2>>"$LOG"
timeout 120 adb push "$TASK/bin/smb_replay_harness.arm64" "$DEV/smb_replay_harness" >/dev/null 2>&1
timeout 120 adb push "$TASK/replay/aot_boot_t1.smbr" "$DEV/aot_boot_t1.smbr" >/dev/null 2>&1
timeout 20 adb shell "chmod 755 $DEV/smb_replay_harness" 2>>"$LOG"
[ -f "$RAW" ] || echo "sweep,condition,sessions,smb_set,nice_set,rep,$(timeout 20 adb shell "$DEV/smb_replay_harness --csv-header" 2>/dev/null | tr -d '\r')" > "$RAW"

# Ordered work list (priority 1a -> 1b -> 1d).  fields: sweep N smb nice rep
LIST=$(mktemp)
{
  for rep in $(seq 1 "$REPS"); do for N in $A_N;  do echo "1a $N 512 0 $rep"; done; done   # 1a
  for rep in $(seq 1 "$REPS"); do for s in $B_SMB; do echo "1b $B_N $s 0 $rep"; done; done  # 1b @N=10
  for rep in $(seq 1 "$REPS"); do for N in $D_N; do for y in $D_NICE; do echo "1d $N 512 $y $rep"; done; done; done  # 1d
} > "$LIST"
total=$(wc -l < "$LIST"); log "--- $total reboots queued (~$(( total*3 ))min) ; 2 datapoints each ---"

i=0
while read -r sweep N smb nice rep <&3; do
  i=$((i+1)); log "[$i/$total]"
  reboot_capture "$sweep" "$N" "$smb" "$nice" "$rep" || true
done 3< "$LIST"
rm -f "$LIST"

log "=== RFC boot DONE: $(($(wc -l < "$RAW")-1)) total raw rows -> $RAW ==="
