# Recording traces on Android boot

Since Android 13 (T), perfetto can be configured to automatically start
recording traces on boot. This can be useful to profile the boot process.

## Steps

* Create a file with the desired [trace configuration](/docs/concepts/config.md)
  in Text format (not binary). Example (more in [/test/configs/](/test/configs/)):
  ```
  # One buffer allocated within the central tracing binary for the entire trace,
  # shared by the two data sources below.
  buffers {
    size_kb: 32768
    fill_policy: DISCARD
  }

  # Ftrace data from the kernel, mainly the process scheduling events.
  data_sources {
    config {
      name: "linux.ftrace"
      target_buffer: 0
      ftrace_config {
        ftrace_events: "sched_switch"
        ftrace_events: "sched_waking"
        ftrace_events: "sched_wakeup_new"

        ftrace_events: "task_newtask"
        ftrace_events: "task_rename"

        ftrace_events: "sched_process_exec"
        ftrace_events: "sched_process_exit"
        ftrace_events: "sched_process_fork"
        ftrace_events: "sched_process_free"
        ftrace_events: "sched_process_hang"
        ftrace_events: "sched_process_wait"
      }
    }
  }

  # Resolve process commandlines and parent/child relationships, to better
  # interpret the ftrace events, which are in terms of pids.
  data_sources {
    config {
      name: "linux.process_stats"
      target_buffer: 0
    }
  }

  # 10s trace, but can be stopped prematurely via `adb shell pkill -u perfetto`.
  duration_ms: 10000
  ```
* Put the file on the device at `/data/misc/perfetto-configs/boottrace.pbtxt`:
  ```
  adb push <yourfile> /data/misc/perfetto-configs/boottrace.pbtxt
  ```
* Enable the `perfetto_trace_on_boot` service:
  ```
  adb shell setprop persist.debug.perfetto.boottrace 1
  ```
  The property is reset on boot. In order to trace the next boot, the command
  must be reissued.
* Reboot the device.
* The output trace will be written at
  `/data/misc/perfetto-traces/boottrace.perfetto-trace`. The file will be
  removed before a new trace is started.
  ```
  adb pull /data/misc/perfetto-traces/boottrace.perfetto-trace
  ```
  **N.B.:** The file will appear after the recording has stopped (be sure to set
  `duration_ms` to a reasonable value in the config) or after the first
  `flush_period_ms`.
* `boottrace.perfetto-trace` can now be opened in
  [ui.perfetto.dev](https://ui.perfetto.dev/)

## Implementation details
* The trace will start only after persistent properties are loaded, which
  happens after /data has been mounted.
* The command to start the trace is implemented as oneshot init service.
