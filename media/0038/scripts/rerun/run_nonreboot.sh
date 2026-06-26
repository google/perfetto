#!/usr/bin/env bash
# RFC rerun — FAITHFUL (duplication) session sweeps, NON-reboot conditions.
#
# Covers RFC data-request 1a/1b/1c/1d for the conditions that need no reboot:
#   idle        — no extra load (control + headline-safe ceiling)
#   dex2oat     — whole-device `compile -m speed -a -f` loop (heavy, sustained)
#   cold_start  — force-stop + `am start` at a fixed offset (repeatable real event)
#
# Model is DUPLICATION (`--dup-mult N` = N concurrent sessions on the real
# timeline), NOT time-warp — this is the model the RFC's "sessions" axis needs.
# Reader confined to traced's cpuset 0-5. All rows append to the shared raw CSV.
#
# Robust for unattended runs: no `set -e`; every device call timeout-wrapped; a
# failed cell is logged and skipped (the median just loses one sample); device is
# cleaned + temp-gated between condition blocks; load is always torn down on exit.
#
#   nohup rfc_rerun/scripts/run_nonreboot.sh > /tmp/rfc_nonreboot.out 2>&1 &
set -uo pipefail
TASK=/home/sashwinbalaji/proto/perfetto/tracing_v2/tasks/task-2-smb-bandwidth
RFC=$TASK/rfc_rerun
DEV=/data/local/tmp/smb
CPUSET=/dev/cpuset/foreground/tasks
PKG=com.google.android.apps.maps
TS=$(date +%Y%m%d_%H%M%S)
OUT=$RFC/results/nonreboot_$TS; mkdir -p "$OUT"
RAW=$RFC/results/rfc_raw.csv          # shared consolidated raw (nonreboot + boot append here)
LOG=$OUT/run.log
REPS=${REPS:-5}; DUR=${DUR:-10}
# Sub-sweep grids (overridable for smoke tests)
A_N="${A_N:-1 2 3 5 7 10 14}"                 # 1a: sessions @512K nice0
B_N="${B_N:-3 5 10}"; B_SMB="${B_SMB:-256 512 1024 2048 4096}"   # 1b: buffer sweep
C_NICE="${C_NICE:-0 5 10}"; C_N="${C_N:-1 3 5 7 10}"             # 1c: idle ceiling grid
D_N="${D_N:-3 5 7 10}"; D_NICE="${D_NICE:-0 -10}"               # 1d: the nice fix (dex2oat)
# NOTE: dex2oat LAST on purpose — `compile -a -f` dispatches an un-killable
# compile-all inside system_server, so we can't cleanly stop it. Running it last
# means the only thing after it is the boot sweep, whose first `adb reboot` wipes
# it; idle/cold_start (measured first) are never contaminated.
CONDS="${CONDS:-idle cold_start dex2oat}"

log(){ echo "[$(date +%H:%M:%S)] $*" | tee -a "$LOG"; }
source "$TASK/scripts/_devstate.sh"   # vskin_c, cool_to, dev_clean (TARGET_C / COOL_TIMEOUT env)

dex_start(){ timeout 20 adb shell "nohup sh -c 'while true; do cmd package compile -m speed -a -f >/dev/null 2>&1; done' >/dev/null 2>&1 &" >/dev/null 2>&1; }
# Robust stop: installd spawns dex2oat un-parented to our loop and the next one
# can start before we kill the last, so kill in a poll-until-dead loop (capped).
dex_count(){ timeout 15 adb shell 'pgrep dex2oat 2>/dev/null | wc -l' 2>/dev/null | tr -d '\r' | tr -dc '0-9'; }
dex_stop(){
  local t0 n; t0=$(date +%s)
  while :; do
    timeout 30 adb shell 'for p in $(pgrep -f "package compile") $(pgrep dex2oat) $(pgrep -f "while true"); do kill -9 $p 2>/dev/null; done' >/dev/null 2>&1
    n=$(dex_count); n=${n:-0}
    { [ "$n" = 0 ] || [ $(( $(date +%s)-t0 )) -ge 45 ]; } && break
    sleep 2
  done
  [ "${n:-0}" != 0 ] && log "  [warn] ${n} dex2oat still alive after 45s kill loop"
}
# Clear ALL leftover load (harness/app/dex2oat) and confirm dex2oat is gone before
# a condition is measured — prevents a stray compile from contaminating it.
clean_wait(){ dev_clean; dex_stop; }
cleanup(){ dex_stop; timeout 20 adb shell "am force-stop $PKG" >/dev/null 2>&1 || true; }
trap cleanup EXIT INT TERM

# Append a raw row: sweep cond sessions smb nice rep  <harness-csv-line>
emit(){ [ -n "$7" ] && echo "$1,$2,$3,$4,$5,$6,$7" >> "$RAW"; }

# Plain duplication run (idle / dex2oat-load-already-on). echoes the harness line.
hrun_dup(){  # N smb nice label
  timeout $((DUR+120)) adb shell "echo \$\$ > $CPUSET 2>/dev/null; exec $DEV/smb_replay_harness \
    --replay $DEV/aot_boot_t1.smbr --multiplier 1 --dup-mult $1 --smb-kb $2 --wake-ms 1 --fill-pct 50 \
    --duration $DUR --reader-nice $3 --label $4 --csv" 2>>"$LOG" | tr -d '\r' | grep ',' | tail -1
}

# Cold-start choreography: each cell triggers its own real launch storm.
cold_dup(){  # N smb nice label
  timeout 20 adb shell "am force-stop $PKG; input keyevent KEYCODE_HOME" >/dev/null 2>&1
  sleep 3
  timeout 15 adb shell 'input keyevent KEYCODE_WAKEUP; wm dismiss-keyguard' >/dev/null 2>&1
  sleep 1
  local outf; outf=$(mktemp)
  ( timeout $((DUR+120)) adb shell "echo \$\$ > $CPUSET 2>/dev/null; exec $DEV/smb_replay_harness \
      --replay $DEV/aot_boot_t1.smbr --multiplier 1 --dup-mult $1 --smb-kb $2 --wake-ms 1 --fill-pct 50 \
      --duration $DUR --reader-nice $3 --label $4 --csv" 2>>"$LOG" | tr -d '\r' | grep ',' | tail -1 > "$outf" ) &
  local hp=$!
  sleep 1
  timeout $((DUR+30)) adb shell "am start -W $PKG" >/dev/null 2>&1 &
  wait "$hp" 2>/dev/null
  local line; line=$(cat "$outf"); rm -f "$outf"
  timeout 15 adb shell "am force-stop $PKG" >/dev/null 2>&1
  echo "$line"
}

# Dispatch one cell by condition. args: sweep cond N smb nice rep
runcell(){
  local sweep=$1 cond=$2 N=$3 smb=$4 nice=$5 rep=$6
  local lbl="${sweep}_${cond}_n${N}_${smb}K_y${nice}_r${rep}" line
  case "$cond" in
    cold_start) line=$(cold_dup "$N" "$smb" "$nice" "$lbl") ;;
    *)          line=$(hrun_dup "$N" "$smb" "$nice" "$lbl") ;;
  esac
  if [ -n "$line" ]; then emit "$sweep" "$cond" "$N" "$smb" "$nice" "$rep" "$line"
  else log "  [warn] no row: $lbl — continuing"; fi
}

medloss(){ # sweep cond N smb nice  -> median loss% over reps (col 11 of harness = field 17 of raw)
  awk -F, -v s="$1" -v c="$2" -v n="$3" -v k="$4" -v y="$5" \
    '$1==s&&$2==c&&$3==n&&$4==k&&$5==y{print $17}' "$RAW" | sort -n \
    | awk '{a[NR]=$1} END{if(NR){if(NR%2)print a[(NR+1)/2]; else printf "%.3f",(a[NR/2]+a[NR/2+1])/2}else print "-"}'
}

# ---------------------------------------------------------------------------
log "=== RFC nonreboot START ts=$TS reps=$REPS dur=${DUR}s conds='$CONDS' out=$OUT ==="
timeout 60 adb wait-for-device 2>/dev/null; adb root >/dev/null 2>&1; timeout 30 adb wait-for-device 2>/dev/null
dev_clean
timeout 20 adb shell "mkdir -p $DEV" 2>>"$LOG"
timeout 120 adb push "$TASK/bin/smb_replay_harness.arm64" "$DEV/smb_replay_harness" >/dev/null 2>&1
timeout 120 adb push "$TASK/replay/aot_boot_t1.smbr" "$DEV/aot_boot_t1.smbr" >/dev/null 2>&1
timeout 20 adb shell "chmod 755 $DEV/smb_replay_harness" 2>>"$LOG"
[ -f "$RAW" ] || echo "sweep,condition,sessions,smb_set,nice_set,rep,$(timeout 20 adb shell "$DEV/smb_replay_harness --csv-header" 2>/dev/null | tr -d '\r')" > "$RAW"

for cond in $CONDS; do
  log "########## CONDITION: $cond ##########"
  clean_wait                                # kill any stray load + wait dex2oat dead
  cool_to                                   # comparable start temperature
  [ "$cond" = dex2oat ] && { dex_start; log "  dex2oat -a -f load started, settling 8s"; sleep 8; }

  # ---- 1a: loss vs sessions @512K, nice 0 ----
  [ "$cond" != dex2oat ] && cool_to  # temp gate (NOT while dex2oat load runs — futile + caps out)
  log "--- [$cond] 1a: sessions @512K ---"
  for N in $A_N; do
    for rep in $(seq 1 "$REPS"); do runcell 1a "$cond" "$N" 512 0 "$rep"; done
    log "  1a $cond N=$N -> median loss=$(medloss 1a "$cond" "$N" 512 0)%"
  done

  # ---- 1b: buffer sweep, nice 0 ----
  [ "$cond" != dex2oat ] && cool_to  # temp gate (skip during dex2oat load)
  log "--- [$cond] 1b: buffer sweep ---"
  for N in $B_N; do
    for smb in $B_SMB; do
      for rep in $(seq 1 "$REPS"); do runcell 1b "$cond" "$N" "$smb" 0 "$rep"; done
    done
    log "  1b $cond N=$N done"
  done

  # ---- 1d: the nice fix (dex2oat only), interleave nice within each rep ----
  if [ "$cond" = dex2oat ]; then
    # no cool here on purpose — the dex2oat load is meant to keep the SoC hot
    log "--- [$cond] 1d: nice fix (0 vs -10) ---"
    for N in $D_N; do
      for rep in $(seq 1 "$REPS"); do
        for nice in $D_NICE; do runcell 1d "$cond" "$N" 512 "$nice" "$rep"; done
      done
      log "  1d $cond N=$N -> nice0=$(medloss 1d "$cond" "$N" 512 0)%  nice-10=$(medloss 1d "$cond" "$N" 512 -10)%"
    done
  fi

  # ---- 1c: idle ceiling grid (idle only) ----
  if [ "$cond" = idle ]; then
    cool_to  # idle, safe to cool
    log "--- [$cond] 1c: ceiling grid (nice 0/5/10) ---"
    for nice in $C_NICE; do
      for N in $C_N; do
        for rep in $(seq 1 "$REPS"); do runcell 1c idle "$N" 512 "$nice" "$rep"; done
      done
      log "  1c nice=$nice done"
    done
  fi

  [ "$cond" = dex2oat ] && dex_stop
  log "########## $cond DONE ($(($(wc -l < "$RAW")-1)) total raw rows) ##########"
done

cleanup
log "=== RFC nonreboot DONE: $(($(wc -l < "$RAW")-1)) total raw rows -> $RAW ==="
