# Android Log

_This data source is supported only on Android userdebug builds._

The "android.log" data source records log events from the Android log
daemon (`logd`). These are the same log messages that are available via
`adb logcat`.

Both textual events and binary-formatted events from the [EventLog] are
supported.

This allows you to see log events time-synced with the rest of the trace. When recording
[long traces](/docs/concepts/config#long-traces), it allows you to record event
logs indefinitely, regardless of the Android log daemon buffer size
(i.e. log events are periodically fetched and copied into the trace buffer).

The data source can be configured to filter event from specific log buffers and
keep only the events matching specific tags or priority.

[EventLog]: https://developer.android.com/reference/android/util/EventLog

### UI

At the UI level, log events are showed in two widgets:

1. A summary track that allows to quickly glance at the distribution of events
   and their severity on the timeline.

2. A table, time-synced with the viewport, that allows to see events within the
   selected time range.

![](/docs/images/android_logs.png "Android logs in the UI")

### SQL

```sql
select l.ts, t.tid, p.pid, p.name as process, l.prio, l.tag, l.msg
from android_logs as l left join thread as t using(utid) left join process as p using(upid)
```
ts | tid | pid | process | prio | tag | msg
---|-----|-----|---------|------|-----|----
291474737298264 | 29128 | 29128 | traced_probes | 4 | perfetto | probes_producer.cc:231 Ftrace setup (target_buf=1)
291474852699265 | 625 | 625 | surfaceflinger | 3 | SurfaceFlinger | Finished setting power mode 1 on display 0
291474853274109 | 1818 | 1228 | system_server | 3 | SurfaceControl | Excessive delay in setPowerMode()
291474882474841 | 1292 | 1228 | system_server | 4 | DisplayPowerController | Unblocked screen on after 242 ms
291474918246615 | 1279 |    1228 | system_server | 4 | am_pss | Pid=28568 UID=10194 Process Name="com.google.android.apps.fitness" Pss=12077056 Uss=10723328 SwapPss=183296 Rss=55021568 StatType=0 ProcState=18 TimeToCollect=51

### TraceConfig

Trace proto:
[AndroidLogPacket](/docs/reference/trace-packet-proto.autogen#AndroidLogPacket)

Config proto:
[AndroidLogConfig](/docs/reference/trace-config-proto.autogen#AndroidLogConfig)

Sample config:

```protobuf
data_sources: {
    config {
        name: "android.log"
        android_log_config {
            min_prio: PRIO_VERBOSE
            filter_tags: "perfetto"
            filter_tags: "my_tag_2"
            log_ids: LID_DEFAULT
            log_ids: LID_RADIO
            log_ids: LID_EVENTS
            log_ids: LID_SYSTEM
            log_ids: LID_CRASH
            log_ids: LID_KERNEL
        }
    }
}
```
