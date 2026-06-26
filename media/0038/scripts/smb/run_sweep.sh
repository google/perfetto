#!/usr/bin/env bash
# tracing-v2 Task 02 — autonomous SMB-size × load × multiplier sweep at REAL
# boot / first-unlock. Self-contained: reboots, thermal-aware cooldown, retries,
# per-run CSV + logs, best-effort trace capture, then charts. Survives the
# session closing (run with nohup). Stops starting new runs after a deadline.
#
#   nohup scripts/run_sweep.sh > /tmp/run_sweep.out 2>&1 &
set -uo pipefail   # deliberately NOT -e: a single bad run must not abort the sweep

TASK=/home/sashwinbalaji/proto/perfetto/tracing_v2/tasks/task-2-smb-bandwidth
BIN_LOCAL=$TASK/bin/smb_replay_harness.arm64
REPLAY_LOCAL=$TASK/replay/aot_boot_t1.smbr
DEV=/data/local/tmp/smb
TS=$(date +%Y%m%d_%H%M%S)
OUT=$TASK/results/sweep_$TS
mkdir -p "$OUT/traces"
CSV=$OUT/sweep.csv
LOG=$OUT/sweep.log
RUN_MINUTES=${RUN_MINUTES:-55}
DEADLINE=$(( $(date +%s) + RUN_MINUTES*60 ))
DUR=${DUR:-12}                 # harness run length per datapoint (s)
COOL_TJ=${COOL_TJ:-40000}      # wait until max core Tj < this (milli-C) before next reboot
COOL_MAX=${COOL_MAX:-150}      # cap the cooldown wait (s)
PKG=com.google.android.apps.maps

log(){ echo "[$(date +%H:%M:%S)] $*" | tee -a "$LOG"; }
source "$(cd "$(dirname "$0")" && pwd)/_devstate.sh"   # vskin_c, cool_to (vskin-based cooldown)
past_deadline(){ [ "$(date +%s)" -ge "$DEADLINE" ]; }

adb_back(){ timeout 200 adb wait-for-device 2>/dev/null; adb root >/dev/null 2>&1; timeout 60 adb wait-for-device 2>/dev/null; }

tj_max(){ adb shell 'cat /dev/thermal/tz-by-name/BIG/temp /dev/thermal/tz-by-name/MID/temp /dev/thermal/tz-by-name/LITTLE/temp 2>/dev/null' 2>/dev/null | tr -d '\r' | sort -n | tail -1; }
vskin(){ adb shell 'logcat -d 2>/dev/null | grep "pixel-thermal: VIRTUAL-SKIN:" | tail -1' 2>/dev/null | sed -nE 's/.*VIRTUAL-SKIN:([0-9.]+) raw.*/\1/p'; }

cooldown(){ cool_to; }   # vskin-based, screen-off, soft cap, never aborts (see _devstate.sh)

wait_boot(){  # boot_completed + harness reachable; timeout ~150s
  local t0=$(date +%s)
  while :; do
    [ "$(adb shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" = "1" ] && \
      adb shell "test -x $DEV/smb_replay_harness && test -e /dev/cpuset/foreground/tasks" 2>/dev/null && return 0
    [ $(( $(date +%s) - t0 )) -ge 150 ] && { log "  WARN wait_boot timeout"; return 1; }
    sleep 1
  done
}

hrun(){  # smb_kb mult label dur -> csv line (confined to traced's cpuset 0-5)
  adb shell "echo \$\$ > /dev/cpuset/foreground/tasks 2>/dev/null; exec $DEV/smb_replay_harness \
    --replay $DEV/aot_boot_t1.smbr --multiplier $2 --smb-kb $1 --wake-ms 1 --fill-pct 50 \
    --duration $4 --reader-nice 0 --label $3 --csv" 2>>"$LOG" | tr -d '\r' | grep -E '^[a-z]' | head -1
}

# datapoint: scenario smb mult rep [trace]
datapoint(){
  local scen=$1 smb=$2 mult=$3 rep=$4 trace=${5:-}
  past_deadline && { log "deadline reached, stop"; return 2; }
  log "RUN $scen smb=${smb}K mult=${mult}x rep=$rep ${trace:+(+trace)}"
  adb reboot 2>>"$LOG"; sleep 4; adb_back
  wait_boot || { log "  boot failed -> skip"; cooldown; return 1; }
  local up; up=$(adb shell cat /proc/uptime 2>/dev/null | tr -d '\r' | cut -d' ' -f1)
  case $scen in
    boot) : ;;
    unlock_home) sleep 1; adb shell 'input keyevent KEYCODE_WAKEUP; wm dismiss-keyguard' >/dev/null 2>&1; sleep 1 ;;
    unlock_app)  sleep 1; adb shell 'input keyevent KEYCODE_WAKEUP; wm dismiss-keyguard' >/dev/null 2>&1
                 adb shell "am start -W $PKG >/dev/null 2>&1 &"; sleep 1 ;;
  esac
  local lbl="${scen}_${smb}K_${mult}x_r${rep}"
  if [ -n "$trace" ]; then
    adb shell "perfetto -o $DEV/tr_$lbl.pftrace -t $((DUR+2))s -b 48mb sched freq idle am wm binder_driver >/dev/null 2>&1 &" 2>>"$LOG"
    sleep 1
  fi
  local line; line=$(hrun "$smb" "$mult" "$lbl" "$DUR")
  if [ -n "$line" ]; then
    echo "$scen,$smb,$mult,$rep,$up,$line" >> "$CSV"
    log "  -> $(echo "$line" | awk -F, '{printf "loss=%s%% in=%s occ=%s%% starved=%s%%",$11,$12,$14,$17}')"
  else
    log "  -> NO OUTPUT"
  fi
  if [ -n "$trace" ]; then
    sleep 3; adb pull "$DEV/tr_$lbl.pftrace" "$OUT/traces/" >/dev/null 2>&1 && log "  trace saved" || log "  trace pull failed"
    adb shell "rm -f $DEV/tr_$lbl.pftrace" >/dev/null 2>&1
  fi
  [ "$scen" = unlock_app ] && adb shell "am force-stop $PKG" >/dev/null 2>&1
  cooldown
}

# ---------------------------------------------------------------------------
log "=== SWEEP START ts=$TS deadline=+${RUN_MINUTES}min out=$OUT ==="
adb_back
adb shell "mkdir -p $DEV" 2>>"$LOG"
adb push "$BIN_LOCAL" "$DEV/smb_replay_harness" >/dev/null 2>&1
adb push "$REPLAY_LOCAL" "$DEV/aot_boot_t1.smbr" >/dev/null 2>&1
adb shell "chmod 755 $DEV/smb_replay_harness" 2>>"$LOG"
# header: outer cols + harness csv header
echo "scenario,smb_kb_o,mult_o,rep,uptime_s,$(adb shell "$DEV/smb_replay_harness --csv-header" 2>/dev/null | tr -d '\r')" > "$CSV"

# --- Phase 0: idle baseline grid (NO reboots; device is calm now) ---
log "--- Phase 0: idle grid (no reboots) ---"
for smb in 256 512 1024 2048 4096; do
  for mult in 1 5 10; do
    past_deadline && break
    line=$(hrun "$smb" "$mult" "idle_${smb}K_${mult}x_r1" "$DUR")
    [ -n "$line" ] && echo "idle,$smb,$mult,1,0,$line" >> "$CSV"
    log "  idle ${smb}K ${mult}x -> $(echo "$line" | awk -F, '{print "loss="$11"% occ="$14"%"}')"
  done
done

# --- Build the prioritized reboot list (adaptive ordering) ---
# Phase A: coarse break-finding at 512K. Phase B: SMB sweep at mult 3 & 5.
# Phase T: 3 trace-captured representative runs. Phase C: reps if time remains.
LIST=$(mktemp)
{
  # A — coarse (find the break per scenario)
  for m in 1 3 5 7; do echo "boot 512 $m 1"; done
  for m in 1 3 5 7; do echo "unlock_app 512 $m 1"; done
  for m in 1 3 5;   do echo "unlock_home 512 $m 1"; done
  # T — representative traces (after we expect the break ~5x)
  echo "boot 512 5 2 trace"
  echo "unlock_app 512 5 2 trace"
  echo "boot 2048 5 2 trace"
  # B — SMB-size sweep at the break multipliers (the main question)
  for smb in 256 1024 2048 4096; do for m in 3 5; do echo "boot $smb $m 1"; done; done
  for smb in 256 1024 2048 4096; do for m in 3 5; do echo "unlock_app $smb $m 1"; done; done
  for smb in 1024 2048 4096;     do echo "unlock_home $smb 5 1"; done
  # C — reps for confidence on key points
  for r in 2 3; do
    echo "boot 512 5 $r"; echo "boot 2048 5 $r"; echo "boot 1024 5 $r"
    echo "unlock_app 512 5 $r"; echo "unlock_app 2048 5 $r"
  done
} > "$LIST"
log "--- reboot list: $(wc -l < "$LIST") configs queued ---"

# Read the list on FD 3 so the adb commands inside datapoint() (which read stdin)
# can't consume the remaining config lines.
while read -r scen smb mult rep trace <&3; do
  past_deadline && { log "deadline — stopping reboot loop"; break; }
  datapoint "$scen" "$smb" "$mult" "$rep" "${trace:-}"
done 3< "$LIST"
rm -f "$LIST"

# --- charts + manifest ---
log "--- generating charts ---"
python3 "$TASK/scripts/plot_sweep.py" "$OUT" >>"$LOG" 2>&1 || log "  (chart step failed; rerun plot_sweep.py $OUT)"
{
  echo "sweep $TS"; echo "rows: $(($(wc -l < "$CSV")-1))"; echo "deadline_min: $RUN_MINUTES"
  echo "device: Pixel Fold (Tensor G2); reader pinned to /foreground cpuset (cpus 0-5)"
  echo "harness: wake=1ms fill=50% dur=${DUR}s; multiplier = N× the real boot rate"
} > "$OUT/MANIFEST.txt"
log "=== SWEEP DONE: $(($(wc -l < "$CSV")-1)) rows -> $OUT ==="
