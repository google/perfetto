#!/usr/bin/env bash
# RFC rerun MASTER — runs the whole faithful (duplication) data collection:
#   phase 1: non-reboot conditions (idle, cold_start, dex2oat) @ 5 reps
#   phase 2: reboot conditions (real_boot + first_unlock co-captured) @ 3 reps
# then refreshes the consolidated CSV + charts. Each phase is independent: if one
# returns nonzero we log it and continue, so a single failure never loses the rest.
# Writes a STATUS file the supervisor (and you) can read at a glance.
#
#   nohup setsid rfc_rerun/scripts/run_all.sh > /tmp/rfc_all.out 2>&1 < /dev/null &
set -uo pipefail
RFC=/home/sashwinbalaji/proto/perfetto/tracing_v2/tasks/task-2-smb-bandwidth/rfc_rerun
SCR=$RFC/scripts
mkdir -p "$RFC/results"
MLOG=$RFC/results/master.log
STATUS=$RFC/results/STATUS.txt
NR_REPS=${NR_REPS:-5}; NR_DUR=${NR_DUR:-10}
BT_REPS=${BT_REPS:-3}; BT_DUR=${BT_DUR:-20}

mlog(){ echo "[$(date '+%m-%d %H:%M:%S')] $*" | tee -a "$MLOG"; }
rawn(){ echo $(( $(wc -l < "$RFC/results/rfc_raw.csv" 2>/dev/null || echo 1) - 1 )); }
refresh(){ python3 "$SCR/aggregate.py" "$RFC" >>"$MLOG" 2>&1 || true; python3 "$SCR/plots.py" "$RFC" >>"$MLOG" 2>&1 || true; }
status(){ { echo "phase: $1"; echo "updated: $(date)"; echo "raw rows so far: $(rawn)"; } > "$STATUS"; }

mlog "########## RFC RERUN MASTER START (nonreboot ${NR_REPS}rep/${NR_DUR}s, boot ${BT_REPS}rep/${BT_DUR}s) ##########"
status "phase 1 (non-reboot) running"

# Background refresher: keep the consolidated CSV + charts + STATUS current every
# 5 min while the run proceeds, so deliverables are never stale even if nobody is
# watching. Self-terminates when the run finishes.
( while [ ! -f "$RFC/results/_MASTER_DONE" ]; do sleep 300; refresh; done ) &
REFRESHER=$!
trap 'kill "$REFRESHER" 2>/dev/null' EXIT

mlog "--- phase 1: non-reboot (idle, cold_start, dex2oat) ---"
REPS=$NR_REPS DUR=$NR_DUR bash "$SCR/run_nonreboot.sh" >>"$MLOG" 2>&1 || mlog "  [warn] non-reboot phase returned nonzero — continuing"
refresh
mlog "--- phase 1 done ($(rawn) raw rows) ---"
status "phase 2 (boot) running"

mlog "--- phase 2: boot (real_boot + first_unlock) ---"
REPS=$BT_REPS DUR=$BT_DUR bash "$SCR/run_boot.sh" >>"$MLOG" 2>&1 || mlog "  [warn] boot phase returned nonzero — continuing"
refresh
status "ALL DONE"

mlog "########## RFC RERUN MASTER DONE: $(rawn) raw rows ##########"
touch "$RFC/results/_MASTER_DONE"
