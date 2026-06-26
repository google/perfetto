#!/usr/bin/env bash
# Shared device-state helpers for the Task-2 on-device sweeps.
#   source "$(cd "$(dirname "$0")" && pwd)/_devstate.sh"
#
# Why: the drain ceiling we measure depends on core frequency, which depends on
# thermal headroom. So every loaded/ceiling run should start from a COMPARABLE
# temperature, and start CLEAN (no orphaned load/harness from a prior run).
#
# Temperature = VIRTUAL-SKIN (the skin temp the platform throttles on; mValue is
# already in °C). We read the LIVE value under "Current temperatures from HAL",
# NOT the stale "Cached temperatures" copy. Hot-throttling on this device starts
# at ~39 °C, so a ~37 °C gate means we start just below throttling.
#
# ROBUSTNESS: nothing here ever aborts a run. Every device read is timeout-wrapped;
# if cooling can't reach the target (or the sensor can't be read) we LOG it and
# CONTINUE. Tune via TARGET_C / COOL_TIMEOUT env.
#
# Provides:  vskin_c  cool_to  dev_clean
TARGET_C=${TARGET_C:-37}             # cool to <= this (°C, vskin) before measuring
COOL_TIMEOUT=${COOL_TIMEOUT:-600}    # SOFT cap on cooling (s); on hit we note + continue

_ds_log(){ if type log >/dev/null 2>&1; then log "$@"; else echo "[$(date +%H:%M:%S)] $*"; fi; }

# Live VIRTUAL-SKIN temperature in °C, or EMPTY if it can't be read. Prefers the
# "Current temperatures from HAL" reading; falls back to the last VIRTUAL-SKIN line.
vskin_c(){
  timeout 20 adb shell 'dumpsys thermalservice 2>/dev/null' 2>/dev/null | tr -d '\r' | awk '
    /Current temperatures from HAL:/ { h=1 }
    /mName=VIRTUAL-SKIN/ && match($0,/mValue=[0-9.]+/) {
      v=substr($0,RSTART+7,RLENGTH-7); last=v; if(h) hal=v
    }
    END { if(hal!="") printf "%.1f", hal+0; else if(last!="") printf "%.1f", last+0 }'
}

# Cool (screen off) until vskin <= TARGET_C, or the soft cap. ALWAYS returns 0
# (never aborts the caller). Sets START_TEMP (a number, or "?" if unreadable).
cool_to(){
  local deadline=$(( $(date +%s) + COOL_TIMEOUT )) t
  timeout 10 adb shell 'input keyevent 223' >/dev/null 2>&1     # 223 = SLEEP: screen off helps it cool
  while :; do
    t=$(vskin_c)
    if [ -z "$t" ]; then START_TEMP="?"; _ds_log "  [cool] vskin unreadable — skipping cooldown, continuing"; return 0; fi
    if awk "BEGIN{exit !($t<=$TARGET_C)}"; then START_TEMP=$t; _ds_log "  [cool] start vskin ${t}C (<= ${TARGET_C}C)"; return 0; fi
    if [ "$(date +%s)" -ge "$deadline" ]; then START_TEMP=$t; _ds_log "  [cool] cooldown cap ${COOL_TIMEOUT}s reached at vskin ${t}C (target ${TARGET_C}C) — noted, continuing"; return 0; fi
    _ds_log "  [cool] vskin ${t}C > ${TARGET_C}C, waiting..."; sleep 15
  done
}

# Kill any leftover harness/load from a prior run so the device starts clean.
dev_clean(){
  timeout 30 adb shell 'for p in $(pgrep -f smb_replay_harness) $(pgrep -f "cmd package compile") $(pgrep -f dex2oat) $(pgrep -f "am start -W") $(pgrep -f "while true"); do kill -9 $p 2>/dev/null; done' >/dev/null 2>&1 || true
}
