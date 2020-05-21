# CPU Scheduling events

On Android and Linux Perfetto can gather scheduler traces via the Linux Kernel
[ftrace](https://www.kernel.org/doc/Documentation/trace/ftrace.txt)
infrastructure.

This allows to get fine grained scheduling events such as:

* Which threads were scheduling on which CPU cores at any point in time, with
  nanosecond accuracy.
* The reason why a running thread got descheduled (e.g. pre-emption, blocked on
  a mutex, blocking syscall or any other wait queue).
* The point in time when a thread became eligible to be executed, even if it was
  not put immediately on any CPU run queue, together with the source thread that
  made it executable.

## UI

When zoomed out, the UI shows a quantized view of CPU usage, which collapses the
scheduling information:

![](/docs/images/cpu-bar-graphs.png "Quantized view of CPU run queues")

However, by zooming in, the individual scheduling events become visible:

![](/docs/images/cpu-zoomed.png "Detailed view of CPU run queues")

Clicking on a CPU slice shows the relevant information in the details panel:

![](/docs/images/cpu-sched-details.png "CPU scheduling details")

Scrolling down, when expanding individual processes, the scheduling events also
create one track for each thread, which allows to follow the evolution of the
state of individual threads:

![](/docs/images/thread-states.png "States of individual threads")


```protobuf
data_sources {
  config {
    name: "linux.ftrace"
    ftrace_config {
      ftrace_events: "sched/sched_switch"
      ftrace_events: "sched/sched_waking"
    }
  }
}
```

## SQL

At the SQL level, the scheduling data is exposed in the
[`sched_slice`](/docs/analysis/sql-tables.autogen#sched_slice) table.

```sql
select ts, dur, cpu, end_state, priority, process.name, thread.name
from sched_slice left join thread using(utid) left join process using(upid)
```

ts | dur | cpu | end_state | priority | process.name, | thread.name
---|-----|-----|-----------|----------|---------------|------------
261187012170995 | 247188 | 2 | S | 130 | /system/bin/logd | logd.klogd
261187012418183 | 12812 | 2 | D | 120 | /system/bin/traced_probes | traced_probes0
261187012421099 | 220000 | 4 | D | 120 | kthreadd | kworker/u16:2
261187012430995 | 72396 | 2 | D | 120 | /system/bin/traced_probes | traced_probes1
261187012454537 | 13958 | 0 | D | 120 | /system/bin/traced_probes | traced_probes0
261187012460318 | 46354 | 3 | S | 120 | /system/bin/traced_probes | traced_probes2
261187012468495 | 10625 | 0 | R | 120 | [NULL] | swapper/0
261187012479120 | 6459 | 0 | D | 120 | /system/bin/traced_probes | traced_probes0
261187012485579 | 7760 | 0 | R | 120 | [NULL] | swapper/0
261187012493339 | 34896 | 0 | D | 120 | /system/bin/traced_probes | traced_probes0

## TraceConfig

```protobuf
data_sources: {
    config {
        name: "linux.ftrace"
        ftrace_config {
            ftrace_events: "sched/sched_switch"
            ftrace_events: "sched/sched_process_exit"
            ftrace_events: "sched/sched_process_free"
            ftrace_events: "task/task_newtask"
            ftrace_events: "task/task_rename"
        }
    }
}

# This is to get full process name and thread<>process relationships.
data_sources: {
    config {
        name: "linux.process_stats"
    }
}
```

## Scheduling wakeups and latency analysis

By further enabling the following in the TraceConfig, the ftrace data source
will record also scheduling wake up events:

```protobuf
  ftrace_events: "sched/sched_wakeup_new"
  ftrace_events: "sched/sched_waking"
```

While `sched_switch` events are emitted only when a thread is in the
`R(unnable)` state AND is running on a CPU run queue, `sched_waking` events are
emitted when any event causes a thread state to change.

Consider the following example:

```
Thread A
condition_variable.wait()
                                     Thread B
                                     condition_variable.notify()
```

When Thread A suspends on the wait() it will enter the state `S(sleeping)` and
get removed from the CPU run queue. When Thread B notifies the variable, the
kernel will transition Thread A into the `R(unnable)` state. Thread A at that
point is eligible to be put back on a run queue. However this might not happen
for some time because, for instance:

* All CPUs might be busy running some other thread, and Thread A needs to wait
  to get a run queue slot assigned (or the other threads have higher priority).
* Some other CPUs other than the current one, but the scheduler load balancer
  might take some time to move the thread on another CPU.

Unless using real-time thread priorities, most Linux Kernel scheduler
configurations are not strictly work-conserving. For instance the scheduler
might prefer to wait some time in the hope that the thread running on the
current CPU goes to idle, avoiding a cross-cpu migration which might be more
costly both in terms of overhead and power.

NOTE: `sched_waking` and `sched_wakeup` provide nearly the same information. The
      difference lies in wakeup events across CPUs, which involve
      inter-processor interrupts. The former is emitted on the source (wakee)
      CPU, the latter on the destination (waked) CPU. `sched_waking` is usually
      sufficient for latency analysis, unless you are looking into breaking down
      latency due to inter-processor signaling.

When enabling `sched_waking` events, the following will appear in the UI when
selecting a CPU slice:

![](/docs/images/latency.png "Scheduling wake-up events in the UI")

