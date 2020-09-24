## Syscalls
The enter and exit of all syscalls can be tracked in Perfetto traces.


The following ftrace events need to added to the trace config to collect syscalls.

```protobuf
data_sources: {
    config {
        name: "linux.ftrace"
        ftrace_config {
            ftrace_events: "raw_syscalls/sys_enter"
            ftrace_events: "raw_syscalls/sys_exit"
        }
    }
}
```

## Linux kernel tracing
Perfetto integrates with [Linux kernel event tracing](https://www.kernel.org/doc/Documentation/trace/ftrace.txt).
While Perfetto has special support for some events (for example see [CPU Scheduling](#cpu-scheduling)) Perfetto can collect arbitrary events.
This config collects four Linux kernel events: 

```protobuf
data_sources {
  config {
    name: "linux.ftrace"
    ftrace_config {
      ftrace_events: "ftrace/print"
      ftrace_events: "sched/sched_switch"
      ftrace_events: "task/task_newtask"
      ftrace_events: "task/task_rename"
    }
  }
}
```

The full configuration options for ftrace can be seen in [ftrace_config.proto](/protos/perfetto/config/ftrace/ftrace_config.proto).

## Android system logs

### Android logcat
Include Android Logcat messages in the trace and view them in conjunction with other trace data.

![](/docs/images/android_logs.png)

You can configure which log buffers are included in the trace. If no buffers are specified, all will be included.

```protobuf
data_sources: {
    config {
        name: "android.log"
        android_log_config {
            log_ids: LID_DEFAULT
            log_ids: LID_SYSTEM
            log_ids: LID_CRASH
        }
    }
}
```

You may also want to add filtering on a tags using the `filter_tags` parameter or set a min priority to be included in the trace using `min_prio`.
For details about configuration options, see [android\_log\_config.proto](/protos/perfetto/config/android/android_log_config.proto). 

The logs can be investigated along with other information in the trace using the [Perfetto UI](https://ui.perfetto.dev) as shown in the screenshot above.

If using the `trace_processor`, these logs will be in the [android\_logs](/docs/analysis/sql-tables.autogen#android_logs) table. To look at the logs with the tag ‘perfetto’ you would use the following query:

```sql
select * from android_logs where tag = "perfetto" order by ts
```

### Android application tracing
You can enable atrace through Perfetto. 

![](/docs/images/userspace.png)

Add required categories to `atrace_categories` and set `atrace_apps` to a specific app to collect userspace annotations from that app.

```protobuf
data_sources: {
    config {
        name: "linux.ftrace"
        ftrace_config {
            atrace_categories: "view"
            atrace_categories: "webview"
            atrace_categories: "wm"
            atrace_categories: "am"
            atrace_categories: "sm"
            atrace_apps: "com.android.phone"
        }
    }
}
```