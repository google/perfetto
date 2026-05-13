# Cookbook: Periodic Trace Snapshots

In this guide, you'll learn how to:

- Run a continuous ring-buffer trace on an Android device or Linux machine.
- Take periodic snapshots of the trace using `--clone-by-name`.
- Analyze each snapshot with Trace Processor to monitor device metrics over
  time.

This workflow is useful when you need to repeatedly observe system metrics
(CPU frequency, power rails, temperatures, etc.) while iterating on device
or system configuration, without restarting tracing each time.

## Use case

Imagine you are tuning device or system parameters (e.g. writing to `/proc` or
`/sys` nodes) and want to see the effect on power, thermals and CPU behavior
within seconds. The traditional workflow of "start trace, stop trace, pull,
analyze" adds unnecessary friction.

With **periodic trace snapshots** you start a single ring-buffer trace once,
then clone it as many times as you like. Each clone is an independent snapshot
of the ring buffer at that point in time; the original trace keeps running
undisturbed.

## Prerequisites

<?tabs>

TAB: Android

- An Android device running Android 14 (U) or later (the `--clone-by-name`
  flag requires the Perfetto v49+ client and service).
- A host machine with `adb` on `PATH` and the device connected via USB.
- `trace_processor_shell` on the host (for analysis). Download prebuilts with:

```bash
curl -LO https://get.perfetto.dev/trace_processor
chmod +x ./trace_processor
```

TAB: Linux

- A Linux machine with Perfetto v49+ installed, or the `tracebox` binary
  downloaded. `tracebox` bundles `traced`, `traced_probes` and the `perfetto`
  client into a single statically linked executable:

```bash
curl -LO https://get.perfetto.dev/tracebox
chmod +x tracebox
```

- `trace_processor_shell` (for analysis). Download prebuilts with:

```bash
curl -LO https://get.perfetto.dev/trace_processor
chmod +x ./trace_processor
```
- Access to `tracefs` for ftrace-based data sources. You do **not** need to run
  as root; instead, `chown` the tracefs directory to your user:

```bash
sudo chown -R $USER /sys/kernel/tracing
```

</tabs?>

## Step 1: Start a ring-buffer trace

<?tabs>

TAB: Android

Create a trace config file `snapshot_config.pbtxt` on the host:

```protobuf
# Identify this session so we can clone it by name later.
unique_session_name: "my_snapshot"

# Use a ring buffer so the trace never stops.
buffers {
  size_kb: 65536
  fill_policy: RING_BUFFER
}

# CPU frequency (event-driven + polling fallback).
data_sources {
  config {
    name: "linux.ftrace"
    ftrace_config {
      ftrace_events: "power/cpu_frequency"
      ftrace_events: "power/cpu_idle"
      ftrace_events: "power/suspend_resume"
      ftrace_events: "thermal/thermal_temperature"
      ftrace_events: "thermal/cdev_update"
    }
  }
}

# Periodic CPU frequency polling (useful on platforms where the ftrace
# event is not emitted).
data_sources {
  config {
    name: "linux.sys_stats"
    sys_stats_config {
      cpufreq_period_ms: 500
    }
  }
}

# Battery counters and power rails (Pixel devices).
data_sources {
  config {
    name: "android.power"
    android_power_config {
      battery_poll_ms: 1000
      battery_counters: BATTERY_COUNTER_CAPACITY_PERCENT
      battery_counters: BATTERY_COUNTER_CHARGE
      battery_counters: BATTERY_COUNTER_CURRENT
      collect_power_rails: true
    }
  }
}
```

Push the config and start tracing:

```bash
adb push snapshot_config.pbtxt /data/misc/perfetto-configs/
adb shell perfetto -c /data/misc/perfetto-configs/snapshot_config.pbtxt --txt \
  --background -o /data/misc/perfetto-traces/snapshot_bg
```

The `--background` flag returns immediately; the trace keeps running in the
ring buffer on the device.

TAB: Linux

Create a trace config file `snapshot_config.pbtxt`:

```protobuf
# Identify this session so we can clone it by name later.
unique_session_name: "my_snapshot"

# Use a ring buffer so the trace never stops.
buffers {
  size_kb: 65536
  fill_policy: RING_BUFFER
}

# CPU frequency (event-driven + polling fallback).
data_sources {
  config {
    name: "linux.ftrace"
    ftrace_config {
      ftrace_events: "power/cpu_frequency"
      ftrace_events: "power/cpu_idle"
      ftrace_events: "power/suspend_resume"
      ftrace_events: "thermal/thermal_temperature"
      ftrace_events: "thermal/cdev_update"
    }
  }
}

# Periodic CPU frequency polling (useful on platforms where the ftrace
# event is not emitted, e.g. Intel CPUs).
data_sources {
  config {
    name: "linux.sys_stats"
    sys_stats_config {
      cpufreq_period_ms: 500
    }
  }
}

# Power monitoring (Chrome OS / Linux).
data_sources {
  config {
    name: "linux.sysfs_power"
  }
}
```

Start the tracing services and begin tracing. If you are using `tracebox`:

```bash
# tracebox starts traced and traced_probes automatically.
./tracebox -c snapshot_config.pbtxt --txt \
  --background -o /tmp/snapshot_bg
```

If you have `traced`, `traced_probes` and `perfetto` installed separately:

```bash
# Ensure traced and traced_probes are running, then:
perfetto -c snapshot_config.pbtxt --txt \
  --background -o /tmp/snapshot_bg
```

The `--background` flag returns immediately; the trace keeps running in the
ring buffer.

</tabs?>

## Step 2: Take a snapshot

Whenever you want to capture the current state of the ring buffer, clone the
session by name:

<?tabs>

TAB: Android

```bash
adb shell perfetto --clone-by-name my_snapshot \
  -o /data/misc/perfetto-traces/snapshot_1.pftrace
```

This creates a read-only copy of the ring buffer contents at that instant. The
original tracing session continues to run. You can repeat this as many
times as you like, giving each snapshot a different output file name:

```bash
# After making a system/device parameter change...
adb shell perfetto --clone-by-name my_snapshot \
  -o /data/misc/perfetto-traces/snapshot_2.pftrace
```

TAB: Linux

```bash
perfetto --clone-by-name my_snapshot \
  -o /tmp/snapshot_1.pftrace
# Or with tracebox:
./tracebox --clone-by-name my_snapshot \
  -o /tmp/snapshot_1.pftrace
```

This creates a read-only copy of the ring buffer contents at that instant. The
original tracing session continues to run. You can repeat this as many
times as you like, giving each snapshot a different output file name:

```bash
# After making a system parameter change...
perfetto --clone-by-name my_snapshot \
  -o /tmp/snapshot_2.pftrace
```

</tabs?>

## Step 3: Pull and analyze a snapshot

<?tabs>

TAB: Android

Pull the snapshot to your host:

```bash
adb pull /data/misc/perfetto-traces/snapshot_1.pftrace /tmp/
```

TAB: Linux

The snapshot is already on the local filesystem at `/tmp/snapshot_1.pftrace`.

</tabs?>

You can analyze the snapshot using the `trace_processor_shell` command line,
the Python API, or by opening it in the
[Perfetto UI](https://ui.perfetto.dev).

### Querying with trace_processor_shell

Run a one-off query directly from the command line using the `query` subcommand:

```bash
trace_processor_shell query /tmp/snapshot_1.pftrace "
  INCLUDE PERFETTO MODULE linux.cpu.frequency;
  SELECT * FROM cpu_frequency_counters LIMIT 100;
"
```

Or open an interactive SQL shell to explore the data:

```bash
trace_processor_shell /tmp/snapshot_1.pftrace
```

Here are some useful queries:

#### CPU frequency

```sql
INCLUDE PERFETTO MODULE linux.cpu.frequency;

SELECT *
FROM cpu_frequency_counters
LIMIT 100;
```

#### Power rails (Android, Pixel devices)

```sql
INCLUDE PERFETTO MODULE android.power_rails;

SELECT *
FROM android_power_rails_counters
LIMIT 100;
```

#### Battery counters (Android)

```sql
SELECT ts, t.name, value
FROM counter AS c
LEFT JOIN counter_track AS t ON c.track_id = t.id
WHERE t.name GLOB 'batt.*';
```

#### Thermal zones

```sql
SELECT ts, t.name, value
FROM counter AS c
LEFT JOIN counter_track AS t ON c.track_id = t.id
WHERE t.name GLOB '*thermal*';
```

### Querying with the Python API

The `perfetto` Python package lets you load traces and query them
programmatically, which is convenient for building custom dashboards or
post-processing data with Pandas / Polars. Install it with:

```bash
pip install perfetto
```

Example:

```python
from perfetto.trace_processor import TraceProcessor

tp = TraceProcessor(trace='/tmp/snapshot_1.pftrace')

# Query CPU frequency as a Pandas DataFrame.
qr = tp.query("""
  INCLUDE PERFETTO MODULE linux.cpu.frequency;
  SELECT cpu, ts, freq
  FROM cpu_frequency_counters
""")
df = qr.as_pandas_dataframe()
print(df.to_string())

# Plot frequency over time for each CPU.
import matplotlib.pyplot as plt
for cpu, group in df.groupby('cpu'):
  plt.plot(group['ts'], group['freq'], label=f'cpu {cpu}')
plt.legend()
plt.xlabel('Timestamp (ns)')
plt.ylabel('Frequency (kHz)')
plt.show()
```

See the [Trace Processor Python docs](/docs/analysis/trace-processor-python.md)
for more details.

If you want to analyze multiple snapshots together,
[Batch Trace Processor](/docs/analysis/batch-trace-processor.md) lets you run a
single query across a set of traces in one go.

## Automating snapshots

A simple shell loop can take a snapshot every N seconds and run a query against
it:

<?tabs>

TAB: Android

```bash
for i in $(seq 1 10); do
  SNAP="/data/misc/perfetto-traces/snap_${i}.pftrace"
  adb shell perfetto --clone-by-name my_snapshot -o "$SNAP"
  adb pull "$SNAP" /tmp/
  echo "=== Snapshot $i ==="
  trace_processor_shell query /tmp/"snap_${i}.pftrace" "
    INCLUDE PERFETTO MODULE linux.cpu.frequency;
    SELECT cpu, avg(freq) AS avg_freq_khz
    FROM cpu_frequency_counters
    GROUP BY cpu;
  "
  sleep 5
done
```

TAB: Linux

```bash
for i in $(seq 1 10); do
  SNAP="/tmp/snap_${i}.pftrace"
  perfetto --clone-by-name my_snapshot -o "$SNAP"
  echo "=== Snapshot $i ==="
  trace_processor_shell query "$SNAP" "
    INCLUDE PERFETTO MODULE linux.cpu.frequency;
    SELECT cpu, avg(freq) AS avg_freq_khz
    FROM cpu_frequency_counters
    GROUP BY cpu;
  "
  sleep 5
done
```

</tabs?>

## Stopping the trace

<?tabs>

TAB: Android

```bash
adb shell killall perfetto
```

TAB: Linux

```bash
killall perfetto
# Or if using tracebox:
killall tracebox
```

</tabs?>

## Limitations and caveats

- **Data source flush intervals**: Not all data sources emit data continuously.
  For example, `android.power` polls at the configured `battery_poll_ms`
  interval, and some data sources only write data on trace start or stop. The
  snapshot will contain whatever has been written to the ring buffer up to that
  point.
- **Ring buffer overwrites**: If the buffer is too small relative to the data
  rate, older data will be overwritten before you snapshot it. Increase
  `size_kb` if you find gaps.
- **Clone availability**: The `--clone-by-name` flag requires Perfetto v49+.
  On Android this means Android 14 (U) or later. On Linux, ensure you are
  using a recent `tracebox` or Perfetto build.
- **Not real-time streaming**: Each snapshot is a point-in-time copy of the
  buffer, not a live stream. There will always be some delay between the last
  event written and the moment you run the clone command.
- **Linux ftrace permissions**: On Linux, ftrace-based data sources require
  access to `tracefs`. Rather than running as root, `chown` the directory to
  your user: `sudo chown -R $USER /sys/kernel/tracing`.
- **Intel CPU frequency**: On most modern Intel CPUs, the `power/cpu_frequency`
  ftrace event is not emitted because frequency scaling is managed internally
  by the CPU. Use the `linux.sys_stats` polling data source with
  `cpufreq_period_ms` as a fallback.
