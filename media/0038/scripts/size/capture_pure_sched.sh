#!/bin/bash
# Capture a PURE high-volume sched-only ftrace trace (theoretical upper bound).
set -u
ROOT=/home/sashwinbalaji/proto/perfetto
OUT=$ROOT/out/linux_clang_release
WORK=/tmp/ftrace_pure
mkdir -p $WORK
export PERFETTO_PRODUCER_SOCK_NAME=$WORK/prod.sock
export PERFETTO_CONSUMER_SOCK_NAME=$WORK/cons.sock
TRACE=$WORK/pure_sched.pftrace

cleanup() {
  kill $LOAD_PID 2>/dev/null
  pkill -f /tmp/sched_load.py 2>/dev/null
  sudo pkill -f "$OUT/traced_probes" 2>/dev/null
  sudo pkill -f "$OUT/traced" 2>/dev/null
}
trap cleanup EXIT

# Pure sched config: ONLY sched_switch + sched_waking (both CompactSched-encoded).
# No atrace/print/irq -> trace is ~100% compact sched -> upper-bound amplification.
cat > $WORK/config.txt <<'EOF'
buffers: { size_kb: 262144 fill_policy: RING_BUFFER }
data_sources: {
  config: {
    name: "linux.ftrace"
    ftrace_config: {
      ftrace_events: "sched/sched_switch"
      ftrace_events: "sched/sched_waking"
      compact_sched: { enabled: true }
    }
  }
}
duration_ms: 10000
EOF

sudo -b env PERFETTO_PRODUCER_SOCK_NAME=$PERFETTO_PRODUCER_SOCK_NAME \
  PERFETTO_CONSUMER_SOCK_NAME=$PERFETTO_CONSUMER_SOCK_NAME \
  $OUT/traced >$WORK/traced.log 2>&1
sleep 1
sudo -b env PERFETTO_PRODUCER_SOCK_NAME=$PERFETTO_PRODUCER_SOCK_NAME \
  $OUT/traced_probes >$WORK/traced_probes.log 2>&1
sleep 1
sudo chmod 0666 $PERFETTO_PRODUCER_SOCK_NAME $PERFETTO_CONSUMER_SOCK_NAME 2>/dev/null

# High context-switch load: 96 pinned ping-pong pairs (4 per CPU) for 12s.
python3 /tmp/sched_load.py 12 96 &
LOAD_PID=$!
sleep 0.5

sudo env PERFETTO_CONSUMER_SOCK_NAME=$PERFETTO_CONSUMER_SOCK_NAME \
  $OUT/perfetto -c $WORK/config.txt --txt -o $TRACE
RC=$?

kill $LOAD_PID 2>/dev/null
pkill -f /tmp/sched_load.py 2>/dev/null
sudo chown $(id -u):$(id -g) $TRACE 2>/dev/null

echo "perfetto rc=$RC"
echo "trace: $TRACE ($(stat -c %s $TRACE 2>/dev/null || echo 0) bytes)"
echo "--- ctxt switches during run (vmstat proxy) ---"
grep ctxt /proc/stat
