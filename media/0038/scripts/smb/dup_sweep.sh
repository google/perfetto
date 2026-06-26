#!/usr/bin/env bash
# tracing-v2 Task 02 — DUPLICATION vs TIME-WARP load model (default nice, idle).
#
# Two ways to mean "N× load":
#   time-warp : play the real stream N× faster (Δt/N) — same bytes, shorter bursts.
#   duplicate : emit N copies per event at the REAL timeline — N× bytes, burst ×N.
# Duplication is the more literal model of de-bundling "more bytes per instant" and
# is HARSHER on buffer capacity (taller bursts). This sweep compares the two at the
# SAME nominal N, default nice 0, idle (healthy ~125 MB/s reader) so we isolate the
# burst/capacity effect from scheduling. It tests the two conclusions duplication
# could overturn: (A) the loss-vs-rate break point, (B) "buffer size barely matters".
#
# Whole harness confined to traced's cpuset 0-5; reader nice 0 (default).
#   nohup scripts/dup_sweep.sh > /tmp/dup_sweep.out 2>&1 &
set -uo pipefail
TASK=/home/sashwinbalaji/proto/perfetto/tracing_v2/tasks/task-2-smb-bandwidth
DEV=/data/local/tmp/smb
CPUSET=/dev/cpuset/foreground/tasks
TS=$(date +%Y%m%d_%H%M%S)
OUT=$TASK/results/dup_$TS; mkdir -p "$OUT"
CSV=$OUT/dup.csv; LOG=$OUT/dup.log
REPS=${REPS:-5}; DUR=${DUR:-10}
A_MULTS="${A_MULTS:-1 3 5 7 10 14}"          # block A: loss vs N, fixed 512K
B_SMBS="${B_SMBS:-256 512 1024 2048 4096}"    # block B: buffer sweep at fixed N
B_MULT="${B_MULT:-10}"
log(){ echo "[$(date +%H:%M:%S)] $*" | tee -a "$LOG"; }

# $1=mode(warp|dup) $2=nominalN $3=smbKB $4=label -> one csv data line
hrun(){ local mflag dflag
  if [ "$1" = warp ]; then mflag=$2; dflag=1; else mflag=1; dflag=$2; fi
  timeout $((DUR+120)) adb shell "echo \$\$ > $CPUSET 2>/dev/null; exec $DEV/smb_replay_harness \
    --replay $DEV/aot_boot_t1.smbr --multiplier $mflag --dup-mult $dflag --smb-kb $3 \
    --wake-ms 1 --fill-pct 50 --duration $DUR --reader-nice 0 --label $4 --csv" \
    2>>"$LOG" | tr -d '\r' | grep ',' | tail -1; }
# CSV cols after 5 prepended (mode,nmult,smb,rep,block): loss=16 in=17 drain=18 occ=19 dup=23
medcol(){ awk -F, -v md="$1" -v m="$2" -v s="$3" -v bl="$4" -v c="$5" \
  '$1==md&&$2==m&&$3==s&&$5==bl{print $c}' "$CSV" | sort -n \
  | awk '{a[NR]=$1} END{if(NR){if(NR%2)print a[(NR+1)/2]; else printf "%.3f",(a[NR/2]+a[NR/2+1])/2}}'; }

log "=== DUP-vs-WARP sweep START reps=$REPS dur=${DUR}s out=$OUT ==="
timeout 60 adb wait-for-device 2>/dev/null; adb root >/dev/null 2>&1; timeout 30 adb wait-for-device 2>/dev/null
source "$(cd "$(dirname "$0")" && pwd)/_devstate.sh"   # vskin_c, cool_to, dev_clean (TARGET_C env)
dev_clean
adb shell "mkdir -p $DEV" 2>>"$LOG"
adb push "$TASK/bin/smb_replay_harness.arm64" "$DEV/smb_replay_harness" >/dev/null 2>&1
adb push "$TASK/replay/aot_boot_t1.smbr" "$DEV/aot_boot_t1.smbr" >/dev/null 2>&1
adb shell "chmod 755 $DEV/smb_replay_harness" 2>>"$LOG"
echo "mode,nmult,smb_kb,rep,block,$(adb shell "$DEV/smb_replay_harness --csv-header" 2>/dev/null | tr -d '\r')" > "$CSV"

# ---- Block A: loss vs N, both modes, fixed 512K ----
cool_to   # comparable start vskin (drain ceiling is frequency/thermal-sensitive)
log "--- BLOCK A: loss vs N @ 512K (warp vs dup) ---"
for m in $A_MULTS; do
  for mode in warp dup; do
    for rep in $(seq 1 "$REPS"); do
      line=$(hrun "$mode" "$m" 512 "A_${mode}_${m}x_r${rep}")
      [ -n "$line" ] && echo "$mode,$m,512,$rep,A,$line" >> "$CSV"
    done
  done
  lw=$(medcol warp "$m" 512 A 16); ld=$(medcol dup "$m" 512 A 16)
  iw=$(medcol warp "$m" 512 A 17); id=$(medcol dup "$m" 512 A 17)
  ow=$(medcol warp "$m" 512 A 19); od=$(medcol dup "$m" 512 A 19)
  log "  ${m}x  loss warp=${lw}% dup=${ld}%   in warp=${iw} dup=${id} MB/s   peakocc warp=${ow}% dup=${od}%"
done

# ---- Block B: buffer-size sweep at fixed N, both modes ----
log "--- BLOCK B: buffer sweep @ ${B_MULT}x (warp vs dup) ---"
for s in $B_SMBS; do
  for mode in warp dup; do
    for rep in $(seq 1 "$REPS"); do
      line=$(hrun "$mode" "$B_MULT" "$s" "B_${mode}_${s}K_r${rep}")
      [ -n "$line" ] && echo "$mode,$B_MULT,$s,$rep,B,$line" >> "$CSV"
    done
  done
  lw=$(medcol warp "$B_MULT" "$s" B 16); ld=$(medcol dup "$B_MULT" "$s" B 16)
  log "  ${s}K @${B_MULT}x  loss warp=${lw}%  dup=${ld}%"
done
log "=== DUP sweep DONE: $(($(wc -l < "$CSV")-1)) rows -> $OUT ==="
