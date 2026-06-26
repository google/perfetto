#!/usr/bin/env bash
# tracing-v2 Task 02 — build the SMB device-stress harness.
#
# Source of truth: tracing_v2/tasks/task-2-smb-bandwidth/src/smb_replay_harness.cc
# It #includes the v2 SharedRingBuffer prototype, so it is built THROUGH the
# dev/primiano/ringbuf worktree (which has the prototype + GN). This script
# syncs the source into the worktree and builds.
#
#   ./build_harness.sh            # host build (x86) for local validation
#   ./build_harness.sh --arm64    # cross-compile for the device (needs NDK)
#
# Output binary is copied to  task-2-smb-bandwidth/bin/.
set -euo pipefail

TASK_DIR="$(cd "$(dirname "$0")/.." && pwd)"
WT="${WT:-/home/sashwinbalaji/proto/perfetto/.claude/worktrees/task4-ringbuf}"
SRC="$TASK_DIR/src/smb_replay_harness.cc"
mkdir -p "$TASK_DIR/bin"

[ -d "$WT" ] || { echo "worktree not found: $WT (set \$WT)"; exit 1; }
cp "$SRC" "$WT/src/tracing/v2/smb_replay_harness.cc"
echo "[build] synced harness source into worktree"

cd "$WT"
if [ "${1:-}" = "--arm64" ]; then
  # Device build. Needs the Android NDK (perfetto: tools/install-build-deps
  # --android -> buildtools/ndk) and an android/arm64 out dir.
  OUT=out/android_arm64
  if [ ! -d "$OUT" ]; then
    mkdir -p "$OUT"
    printf 'target_os = "android"\ntarget_cpu = "arm64"\nis_debug = false\nskip_buildtools_check = true\n' > "$OUT/args.gn"
  fi
  tools/gn gen "$OUT" >/dev/null
  tools/ninja -C "$OUT" smb_replay_harness
  cp "$OUT/smb_replay_harness" "$TASK_DIR/bin/smb_replay_harness.arm64"
  echo "[build] -> $TASK_DIR/bin/smb_replay_harness.arm64 (push to device)"
else
  OUT=out/linux_clang_release
  tools/gn gen "$OUT" >/dev/null
  tools/ninja -C "$OUT" smb_replay_harness
  cp "$OUT/smb_replay_harness" "$TASK_DIR/bin/smb_replay_harness.host"
  echo "[build] -> $TASK_DIR/bin/smb_replay_harness.host (local validation)"
fi
