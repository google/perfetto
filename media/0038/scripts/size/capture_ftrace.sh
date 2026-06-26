#!/bin/bash
# Capture a sched-heavy local ftrace trace on this Linux box.
set -u
ROOT=/home/sashwinbalaji/proto/perfetto
OUT=$ROOT/out/linux_clang_release
WORK=/tmp/ftrace_cap
mkdir -p $WORK
export PERFETTO_PRODUCER_SOCK_NAME=$WORK/prod.sock
export PERFETTO_CONSUMER_SOCK_NAME=$WORK/cons.sock
TRACE=$WORK/local_sched.pftrace

cleanup() {
  sudo pkill -f "$OUT/traced_probes" 2>/dev/null
  sudo pkill -f "$OUT/traced" 2>/dev/null
  pkill -P $$ 2>/dev/null
  kill $LOAD_PIDS 2>/dev/null
}
trap cleanup EXIT

# --- config ---
cat > $WORK/config.txt <<'EOF'
buffers: { size_kb: 131072 fill_policy: RING_BUFFER }
data_sources: {
  config: {
    name: "linux.ftrace"
    ftrace_config: {
      ftrace_events: "sched/sched_switch"
      ftrace_events: "sched/sched_waking"
      ftrace_events: "sched/sched_wakeup"
      ftrace_events: "sched/sched_wakeup_new"
      ftrace_events: "task/task_newtask"
      ftrace_events: "task/task_rename"
      ftrace_events: "irq/irq_handler_entry"
      ftrace_events: "irq/irq_handler_exit"
      compact_sched: { enabled: true }
    }
  }
}
duration_ms: 8000
EOF

# --- start daemons (root for ftrace access) ---
sudo -b env PERFETTO_PRODUCER_SOCK_NAME=$PERFETTO_PRODUCER_SOCK_NAME \
  PERFETTO_CONSUMER_SOCK_NAME=$PERFETTO_CONSUMER_SOCK_NAME \
  $OUT/traced >$WORK/traced.log 2>&1
sleep 1
sudo -b env PERFETTO_PRODUCER_SOCK_NAME=$PERFETTO_PRODUCER_SOCK_NAME \
  $OUT/traced_probes >$WORK/traced_probes.log 2>&1
sleep 1
sudo chmod 0666 $PERFETTO_PRODUCER_SOCK_NAME $PERFETTO_CONSUMER_SOCK_NAME 2>/dev/null

# --- scheduler load: many wakeup-heavy + cpu-hog workers ---
LOAD_PIDS=""
for i in $(seq 1 4); do ( yes >/dev/null ) & LOAD_PIDS="$LOAD_PIDS $!"; done
for i in $(seq 1 48); do ( while true; do sleep 0.001; done ) & LOAD_PIDS="$LOAD_PIDS $!"; done

# --- record (consumer as root to reach socket) ---
sudo env PERFETTO_CONSUMER_SOCK_NAME=$PERFETTO_CONSUMER_SOCK_NAME \
  $OUT/perfetto -c $WORK/config.txt --txt -o $TRACE
RC=$?

kill $LOAD_PIDS 2>/dev/null
sudo chown $(id -u):$(id -g) $TRACE 2>/dev/null

echo "perfetto rc=$RC"
echo "trace: $TRACE ($(stat -c %s $TRACE 2>/dev/null || echo 0) bytes)"
echo "--- traced.log tail ---"; tail -3 $WORK/traced.log
echo "--- traced_probes.log tail ---"; tail -3 $WORK/traced_probes.log
