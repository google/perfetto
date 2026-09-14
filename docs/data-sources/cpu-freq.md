# CPU frequency and idle states

This data source is available on Linux and Android (since P). It records changes
in the CPU power management scheme through the Linux kernel ftrace
infrastructure. It involves three aspects:

#### Frequency scaling

There are two ways to get CPU frequency data:

1. Enabling the `power/cpu_frequency` ftrace event. (See
   [TraceConfig](#traceconfig) below). This will record an event every time the
   in-kernel cpufreq scaling driver changes the frequency. This is not supported
   on all platforms. In our experience it works reliably on ARM-based SoCs but
   produces no data on most modern Intel-based platforms. This is because recent
   Intel CPUs use an internal DVFS which is directly controlled by the CPU, and
   that doesn't expose frequency change events to the kernel. Even on ARM-based
   platforms, the event is emitted only when a CPU frequency changes. In many
   cases the CPU frequency won't change for several seconds, which will show up
   as an empty block at the start of the trace. We suggest always combining this
   with polling (below) to get a reliable snapshot of the initial frequency.
2. Polling sysfs by enabling the `linux.sys_stats` data source and setting
   `cpufreq_period_ms` to a value > 0. This will periodically poll
   `/sys/devices/system/cpu/cpu*/cpufreq/scaling_cur_freq` and record the
   current value in the trace buffer. Works on both Intel and ARM-based
   platforms.

On most Android devices the frequency scaling is per-cluster (group of
big/little cores) so it's not unusual to see groups of four CPUs changing
frequency at the same time.

#### Available frequencies

Enable the `linux.system_info` data source to record the full list of
frequencies supported by each CPU once, at the start of the trace. It reads
`/sys/devices/system/cpu/cpu*/cpufreq/scaling_available_frequencies`. This
information is typically used to tell apart big/little cores by inspecting the
[`cpu_freq` table](/docs/analysis/sql-tables.autogen#cpu_freq).

This is not supported on modern Intel platforms for the same reasons as
`power/cpu_frequency`.

#### Idle states

When no threads are eligible to be executed (e.g. they are all in sleep states)
the kernel sets the CPU into an idle state, turning off some of the circuitry
to reduce idle power usage. Most modern CPUs have more than one idle state:
deeper idle states use less power but also require more time to resume from.

Idle transitions are relatively fast and cheap: a CPU can enter and leave idle
states hundreds of times per second. Idleness differs from full device suspend,
a stronger and more invasive power-saving state (see below). CPUs can be idle
even when the screen is on and the device looks operational.

The number and meaning of available idle states depend heavily on the CPU/SoC.
At the trace level, idle state 0 means not-idle. Values greater than 0
represent increasingly deeper power saving states (e.g., single core idle ->
full package idle).

Note that most Android devices won't enter idle states as long as the USB
cable is plugged in (the USB driver stack holds wakelocks). It is not unusual
to see only one idle state in traces collected through USB.

On most SoCs the frequency has little value when the CPU is idle, as the CPU is
typically clock-gated in idle states. In those cases the frequency in the trace
happens to be the last frequency the CPU was running at before becoming idle.

Known issues:

* The event is emitted only when the frequency changes. This might not happen
  for long periods of time. In short traces it's possible that some CPUs might
  not report any event, showing a gap on the left-hand side of the trace, or
  none at all. Perfetto doesn't currently record the initial CPU frequency when
  the trace is started.

### UI

In the UI, CPU frequency and idleness share a track. The track's height
represents frequency, and its color represents idle state (colored: not-idle,
gray: idle). Hover over or click a point in the track to see both values:
  
![](/docs/images/cpu-frequency.png "CPU frequency and idle states in the UI")

### SQL

In SQL, both frequency and idle states are modeled as counters. The cpuidle
value 0xffffffff (4294967295) means _back to not-idle_.

```sql
select ts, t.name, cpu, value from counter as c
left join cpu_counter_track as t on c.track_id = t.id
where t.name = 'cpuidle' or t.name = 'cpufreq'
```

ts | name | cpu | value
---|------|------|------
261187013242350 | cpuidle | 1 | 0
261187013246204 | cpuidle | 1 | 4294967295
261187013317818 | cpuidle | 1 | 0
261187013333027 | cpuidle | 0 | 0
261187013338287 | cpufreq | 0 | 1036800
261187013357922 | cpufreq | 1 | 1036800
261187013410735 | cpuidle | 1 | 4294967295
261187013451152 | cpuidle | 0 | 4294967295
261187013665683 | cpuidle | 1 | 0
261187013845058 | cpufreq | 0 | 1900800

You can query the list of known CPU frequencies using the
[`cpu_freq` table](/docs/analysis/sql-tables.autogen#cpu_freq).

### TraceConfig

```protobuf
# Event-driven recording of frequency and idle state changes.
data_sources: {
    config {
        name: "linux.ftrace"
        ftrace_config {
            ftrace_events: "power/cpu_frequency"
            ftrace_events: "power/cpu_idle"
            ftrace_events: "power/suspend_resume"
        }
    }
}

# Polling the current cpu frequency.
data_sources: {
    config {
        name: "linux.sys_stats"
        sys_stats_config {
            cpufreq_period_ms: 500
        }
    }
}

# Reporting the list of available frequency for each CPU.
data_sources {
    config {
        name: "linux.system_info"
    }
}
```

### Full-device suspend

Full device suspend happens when a laptop is put in "sleep" mode (e.g. by
closing the lid) or when a smartphone display is turned off for enough time.

When the device is suspended, most hardware units turn off, entering the highest
power-saving state possible (other than full shutdown).

Note that most Android devices don't suspend immediately after dimming the
display but tend to do so if the display is forced off through the power button.
The details are highly device/manufacturer/kernel specific.

Known issues:

* The UI doesn't clearly display the suspended state. When an Android device
  suspends, it looks as if all CPUs are running the kmigration thread and one
  CPU is running the power HAL.