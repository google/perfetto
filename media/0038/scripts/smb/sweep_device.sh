#!/usr/bin/env bash
# tracing-v2 Task 02 — on-device SMB stress sweep (the 2-D loss surface).
#
# Plots loss% vs write-multiplier (1x..), one curve per SYSTEM-LOAD level, with
# the whole harness confined to traced's REAL scheduling domain so the reader
# experiences real scheduling + starvation.
#
# Device facts (Pixel Fold / Tensor G2, measured):
#   - traced AND traced_probes live in cpuset /foreground = cpus 0-5
#     (little 0-3 + mid 4-5), nice 0, NEVER the big cores 6-7.
#   - So we put the ENTIRE harness process in /dev/cpuset/foreground/tasks:
#     writers + reader all float on 0-5, exactly like the real tracing stack,
#     and the system-load stressors contend for those same cores.
#
# Prereqs: ./build_harness.sh --arm64 ; ./build_replay.py -> replay/*.smbr ; adb root.
set -uo pipefail   # NO -e: a transient adb/command failure must not abort an unattended run
TASK_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$TASK_DIR/bin/smb_replay_harness.arm64"
REPLAY="$TASK_DIR/replay/aot_boot_t1.smbr"
DEV=/data/local/tmp/smb
CPUSET=/dev/cpuset/foreground/tasks   # traced's domain = cpus 0-5
OUTCSV="$TASK_DIR/results/device_sweep.csv"
MULTIPLIERS="${MULTIPLIERS:-1 2 3 4 5 7 10 14 20}"
DUR="${DUR:-4}"
LOAD_PKG="${LOAD_PKG:-}"   # app for dex2oat / app-storm; auto-picked if empty

[ -f "$BIN" ] || { echo "build first: scripts/build_harness.sh --arm64"; exit 1; }

# Confine + run the harness in traced's cpuset (echo the shell pid into the
# cpuset, then exec so the harness keeps that scheduling domain).
hrun() { timeout "${HRUN_TO:-300}" adb shell "echo \$\$ > $CPUSET 2>/dev/null; exec $DEV/smb_replay_harness $*"; }

pick_pkg() {
  [ -n "$LOAD_PKG" ] && { echo "$LOAD_PKG"; return; }
  for p in com.google.android.apps.maps com.android.chrome com.google.android.gm \
           com.android.settings; do
    adb shell pm list packages | grep -q "package:$p$" && { echo "$p"; return; }
  done
  echo com.android.settings
}

load_start() {  # idle|dex2oat|appstorm|combined
  case "$1" in
    idle) : ;;
    dex2oat)  adb shell "nohup sh -c 'while true; do cmd package compile -m speed -a -f >/dev/null 2>&1; done' >/dev/null 2>&1 &" ;;
    appstorm) adb shell "nohup sh -c 'while true; do am start -W $PKG >/dev/null 2>&1; sleep 0.4; am force-stop $PKG; done' >/dev/null 2>&1 &" ;;
    combined) load_start dex2oat; load_start appstorm ;;
  esac
}
load_stop() { adb shell "pkill -f 'cmd package compile' 2>/dev/null; pkill -f dex2oat 2>/dev/null; pkill -f 'while true' 2>/dev/null; am force-stop $PKG 2>/dev/null" >/dev/null 2>&1 || true; }
source "$(cd "$(dirname "$0")" && pwd)/_devstate.sh"   # vskin_c, cool_to, dev_clean (TARGET_C env)
trap load_stop EXIT INT TERM

PKG=$(pick_pkg); echo "[sweep] load app = $PKG"
dev_clean                                   # start clean
adb shell "mkdir -p $DEV"
adb push "$BIN" "$DEV/smb_replay_harness" >/dev/null
adb push "$REPLAY" "$DEV/aot_boot_t1.smbr" >/dev/null
adb shell "chmod 755 $DEV/smb_replay_harness"

hrun "--csv-header" | tr -d '\r' > "$OUTCSV"
for LOAD in idle dex2oat appstorm combined; do
  echo "=== system-load = $LOAD ==="
  cool_to                                   # comparable start vskin for each load level
  load_start "$LOAD"; sleep 2
  for M in $MULTIPLIERS; do
    LINE=$(hrun "--replay $DEV/aot_boot_t1.smbr --multiplier $M --smb-kb 512 \
      --wake-ms 1 --fill-pct 50 --duration $DUR --reader-nice 0 \
      --label ${LOAD}_${M}x --csv" 2>/dev/null | tr -d '\r' | grep -E '^[a-z]')
    echo "$LINE" | tee -a "$OUTCSV"
  done
  load_stop; sleep 2
done
echo "[sweep] done -> $OUTCSV"
echo "Plot loss_rate_pct vs multiplier (one line per load); overlay reader_wait_pct."
