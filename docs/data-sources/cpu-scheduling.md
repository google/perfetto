# CPU scheduling (`linux.ftrace`)

Records every context switch and thread wakeup from the Linux kernel scheduler
through [ftrace](https://www.kernel.org/doc/Documentation/trace/ftrace.txt).
Enable it to see which thread ran on which CPU at any moment and why each thread
stopped running (preempted, sleeping, blocked on I/O). You can also see which
thread woke it up and how long it then waited for a CPU.

| | |
|---|---|
| Data source | `linux.ftrace` with the `sched/*` and `task/*` events below |
| Producer | `traced_probes` |
| Platforms | Android 9 (P) and later. Linux with ftrace (`tracefs` mounted at `/sys/kernel/tracing` or `/sys/kernel/debug/tracing`) |
| Privileges | Android: none beyond `adb shell`. Linux: `traced_probes` (or `tracebox`) needs write access to `tracefs`, usually root |
| Config | [FtraceConfig](/docs/reference/trace-config-proto.autogen#FtraceConfig) |
| Trace packets | [FtraceEventBundle](/docs/reference/trace-packet-proto.autogen#FtraceEventBundle) (`TracePacket.ftrace_events`), [FtraceStats](/docs/reference/trace-packet-proto.autogen#FtraceStats) (`TracePacket.ftrace_stats`) |
| Overhead | Among the highest-rate ftrace events: thousands of events per second per CPU on a busy device. Use the default compact encoding and size the buffers accordingly |

## Configuration

Minimal config:

```protobuf
data_sources: {
  config {
    name: "linux.ftrace"
    ftrace_config {
      ftrace_events: "sched/sched_switch"
      ftrace_events: "sched/sched_waking"
      ftrace_events: "sched/sched_process_free"
      ftrace_events: "task/task_newtask"
      ftrace_events: "task/task_rename"
    }
  }
}

# Process names and thread<>process association.
data_sources: {
  config {
    name: "linux.process_stats"
  }
}
```

Relevant `FtraceConfig` fields:

| Field | Default | Description |
|---|---|---|
| `ftrace_events: "sched/sched_switch"` | off | Context switches. Required: produces CPU slices and the Running / end states of threads. |
| `ftrace_events: "sched/sched_waking"` | off | Wakeups, emitted in the waker's context. Produces the Runnable state, the waker thread and scheduling latency. |
| `ftrace_events: "sched/sched_wakeup"` | off | Emitted when the wakee is enqueued. Not used by the importer; rows appear only in the `ftrace_event` table. |
| `ftrace_events: "sched/sched_wakeup_new"` | off | First wakeup of a new task. Not used by the importer; `task/task_newtask` provides the initial Runnable state. |
| `ftrace_events: "sched/sched_blocked_reason"` | off | Android kernels only. Fills `io_wait` and, with `symbolize_ksyms`, `blocked_function`. |
| `ftrace_events: "sched/sched_process_exit"` | off | Recorded, but not used by the importer. |
| `ftrace_events: "sched/sched_process_free"` | off | Marks the end of a thread's lifetime (`thread.end_ts`). |
| `ftrace_events: "task/task_newtask"` | off | Thread and process creation, with the parent thread as waker. |
| `ftrace_events: "task/task_rename"` | off | Thread name changes. |
| `compact_sched.enabled` | `true` (perfetto v42+) | Encodes `sched_switch` and `sched_waking` in a denser format. Compact `sched_waking` also requires `sched_switch`. |
| `symbolize_ksyms` | `false` | Resolves kernel addresses via `/proc/kallsyms`. Needed for `blocked_function`. Requires root or a lowered `kptr_restrict` (on Android, userdebug/eng builds). |
| `buffer_size_kb` | 2048, or 8192 on devices with 7 GB+ of RAM | Size of each per-CPU kernel ring buffer. |
| `drain_period_ms` | chosen by `traced_probes` | How often `traced_probes` reads the kernel buffers. |

## Recording

- **Perfetto UI:** Record new trace > CPU > **Scheduling details**. It also
  enables `sched_wakeup`, `sched_wakeup_new`, `sched_blocked_reason`,
  `sched_process_exit` and `power/suspend_resume`, and turns on process
  association.
- **`perfetto` command (Android):**
  `adb shell perfetto -o /data/misc/perfetto-traces/trace -t 10s sched`. The
  `sched` category enables `sched_switch`, `sched_waking`,
  `sched_blocked_reason`, `sched_cpu_hotplug`, `sched_pi_setprio`,
  `sched_process_exit`, `task_newtask`, `task_rename`, `cgroup/*` and
  `oom/oom_score_adj_update`.
- **`tracebox` (Linux):**
  `sudo ./tracebox -t 10s -o trace sched/sched_switch sched/sched_waking`.
- **Config file:** use the config above, or
  [`test/configs/scheduling.cfg`](/test/configs/scheduling.cfg).

See [Record your first system trace](/docs/getting-started/system-tracing.md) for the
end-to-end steps.

## Trace data

### In the UI

- **CPU Scheduling** group: one **CPU N Scheduling** track per CPU (with the
  core type, e.g. `(big)`, when known). Each slice is a thread running on that
  CPU.

  ![](/docs/images/cpu-zoomed.png "CPU scheduling tracks")

- **Scheduler** group: **Runnable thread count**, **Uninterruptible Sleep
  thread count** and **Active CPU count** tracks.
- **Thread state tracks**: inside each process group, one track per thread
  (named `<thread name> <tid>`) showing Running, Runnable, Sleeping and so on.

  ![](/docs/images/thread-states.png "Thread state tracks")

- **Selecting a CPU slice** opens **CPU Sched Slice**: process, thread,
  cmdline, start time, duration, priority, end state and SQL ID. With
  `sched_waking`, a **Scheduling Latency** section shows the wakeup time and
  the waker thread, and the timeline draws a marker on the waker's CPU and an
  arrow spanning the latency.

  ![](/docs/images/cpu-sched-details.png "CPU Sched Slice details")

  ![](/docs/images/latency.png "Scheduling latency in the timeline")

- **Selecting a thread state** opens **Thread State**: start time, duration,
  state, blocked function, process, thread, priority, previous and next
  state, **Woken by**, **Woken threads**, and a link to the CPU slice.

### In SQL

The [`sched`](/docs/analysis/sql-tables.autogen#sched) table has one row per
CPU slice:

| Column | Description |
|---|---|
| `ts`, `dur` | Start and duration of the slice, in nanoseconds. |
| `cpu`, `ucpu` | CPU number; unique CPU id across machines. |
| `utid` | Thread (join with `thread`). |
| `end_state` | Why the thread stopped running. See [Thread end states](#decoding-end-state). |
| `priority` | Kernel priority the thread ran at. |

The [`thread_state`](/docs/analysis/sql-tables.autogen#thread_state) table has
one row per state interval of each thread. `state` is `Running` or one of the
end state codes. It adds `io_wait`, `blocked_function`, `waker_utid`,
`waker_id` and `irq_context`. Every `sched` row has a matching `thread_state`
row with `state = 'Running'`.

NOTE: `sched_slice` is a legacy alias of `sched`. Use `sched` in new queries.

CPU time per thread:

```sql
SELECT
  process.name AS process_name,
  thread.name AS thread_name,
  thread.tid,
  SUM(sched.dur) AS cpu_time_ns,
  COUNT(*) AS slices
FROM sched
JOIN thread USING (utid)
LEFT JOIN process USING (upid)
WHERE NOT thread.is_idle AND sched.dur > 0
GROUP BY utid
ORDER BY cpu_time_ns DESC
LIMIT 5;
```

| process_name | thread_name | tid | cpu_time_ns | slices |
|---|---|---|---|---|
| com.android.chrome:sandboxed_process0 | CrRendererMain | 5363 | 2272827323 | 1581 |
| /system/bin/traced_probes | traced_probes | 906 | 1495805120 | 2306 |
| com.android.chrome | .android.chrome | 5313 | 1442039916 | 3571 |
| com.android.chrome:privileged_process0 | CrGpuMain | 5395 | 1384421474 | 2837 |
| kswapd0 | kswapd0 | 150 | 1377437355 | 1967 |

For wakeup latency, runnable time and blocking analysis, see the `sched.*`
modules in the
[Standard library](/docs/analysis/stdlib-docs.autogen).

### Stats

Relevant rows in the `stats` table:

| Name | Meaning |
|---|---|
| `ftrace_cpu_has_data_loss` | Indexed by CPU. The kernel overwrote events before `traced_probes` read them; scheduling data for that CPU is unreliable. |
| `ftrace_cpu_overrun_delta` | Indexed by CPU. Number of events lost to kernel ring buffer overruns during the trace. |
| `ftrace_setup_errors` | One or more requested events or categories failed to enable. |
| `mismatched_sched_switch_tids` | A `sched_switch` switched out a thread other than the one last switched in on that CPU, usually because of data loss. |
| `compact_sched_has_parse_errors` | The compact sched data could not be fully decoded. |

## {#decoding-end-state} Thread end states

`sched.end_state` and non-running `thread_state.state` values are one or more
kernel task state characters. The UI translates them as follows:

| Code | Translation |
|---|---|
| `R` | Runnable |
| `R+` | Runnable (Preempted) |
| `S` | Sleeping |
| `D` | Uninterruptible Sleep |
| `T` | Stopped |
| `t` | Traced |
| `X` | Exit (Dead) |
| `Z` | Exit (Zombie) |
| `x` | Task Dead |
| `I` | Idle |
| `K` | Wake Kill |
| `W` | Waking |
| `P` | Parked |
| `N` | No Load |

Multiple characters combine, e.g. `DK` is Uninterruptible Sleep + Wake Kill.
Not all combinations are meaningful.

If the trace ends while a thread is still running, its last slice has
`end_state` NULL and `dur` -1.

## Limitations

- Without `sched_waking`, a sleeping thread goes straight to Running: there is
  no Runnable state, no waker and no latency information.
- `sched_blocked_reason` exists only on Android kernels, so `io_wait` and
  `blocked_function` are NULL on upstream Linux.
- Wakeups from interrupt context have no waker thread. On traces without
  `irq_context`, the UI shows "Woken by (maybe interrupt)".
- Thread and process names come from `task_newtask`, `task_rename` and
  `linux.process_stats`. Without them, tracks show only the tid.
- The kernel ring buffers are shared by all concurrent ftrace sessions;
  `buffer_size_kb` is not guaranteed when other sessions are active.

NOTE: A thread can stay Runnable after its wakeup because every CPU is busy
with threads of equal or higher priority, or because the load balancer has not
yet moved it to an idle CPU. Outside real-time priorities, the Linux scheduler
is not strictly work-conserving and may wait rather than migrate a thread.
`sched_waking` is emitted in the waker's context, when the wakeup starts;
`sched_wakeup` is emitted when the wakee is enqueued, possibly on the wakee's
target CPU. `sched_waking` is sufficient for latency analysis.

## Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| No CPU Scheduling tracks | `sched/sched_switch` not enabled, or tracefs not writable | Add the event. On Linux, run `tracebox` / `traced_probes` as root. Check `ftrace_setup_errors` in `stats`. |
| Gaps in CPU tracks, `ftrace_cpu_has_data_loss` set | Kernel buffer overwritten between reads | Increase `buffer_size_kb`, lower `drain_period_ms`, or enable fewer events. See [Buffers and data flow](/docs/concepts/buffers.md). |
| Threads have no Runnable state and no waker | `sched/sched_waking` not enabled | Add `sched/sched_waking`. |
| Tracks named only by tid, `process.name` NULL | No process association | Add `linux.process_stats` and `task/task_newtask`, `task/task_rename`. |
| `blocked_function` always NULL | Missing `sched_blocked_reason` or kernel symbols | Enable `sched/sched_blocked_reason` and `symbolize_ksyms` (Android userdebug/eng). |
| Recording fails on Android 9 or 10 | Tracing services disabled | `adb shell setprop persist.traced.enable 1`. |

## See also

- [Record your first system trace](/docs/getting-started/system-tracing.md)
- [Profile and trace native code on Linux](/docs/getting-started/linux-cookbook.md#blocked-thread)
- [Case study: a SystemUI scheduling blockage](/docs/case-studies/scheduling-blockages.md)
- [Buffers and data flow](/docs/concepts/buffers.md)
