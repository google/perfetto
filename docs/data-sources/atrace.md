# ATrace: Android system and app trace events

On Android, native and managed apps can inject custom slices and counter trace
points into the trace. This is possible through the following:

* Java/Kotlin apps (SDK): `android.os.Trace`.
  See https://developer.android.com/reference/android/os/Trace.

* Native processes (NDK): `ATrace_beginSection() / ATrace_setCounter()` defined
  in `<trace.h>`. See https://developer.android.com/ndk/reference/group/tracing.

* Android internal processes: `ATRACE_BEGIN()/ATRACE_INT()` defined in
  [`libcutils/trace.h`][libcutils].

This API has been available since Android 4.3 (API level 18) and predates
Perfetto. All these annotations, which internally are all routed through the
internal libcutils API, are and will continue to be supported by Perfetto.

There are two types of atrace events: System and App events.

**System events**: are emitted only by Android internals using libcutils.
These events are grouped in categories (also known as _tags_), e.g.
"am" (ActivityManager), "pm" (PackageManager).
For a full list of categories see the _Record new trace_ page of the
[Perfetto UI](https://ui.perfetto.dev).

Categories can be used to enable group of events across several processes,
without having to worry about which particular system process emits them.

**App events**: have the same semantics of system events. Unlike system events,
however, they don't have any tag-filtering capability (all app events share the
same tag `ATRACE_TAG_APP`) but can be enabled on a per-app basis.

See the [TraceConfig](#traceconfig) section below for instructions on how to
enable both system and app events.

#### Instrumentation overhead

ATrace instrumentation a non-negligible cost of 1-10us per event.
This is because each event involves a stringification, a JNI call if coming from
a managed execution environment, and a user-space <-> kernel-space roundtrip to
write the marker into `/sys/kernel/debug/tracing/trace_marker` (which is the
most expensive part).

Our team is looking into a migration path for Android, in light of the newly
introduced [Tracing SDK](/docs/instrumentation/tracing-sdk.md). At the moment
the advice is to keep using the existing ATrace API on Android.

[libcutils]: https://cs.android.com/android/platform/superproject/main/+/main:system/core/libcutils/include/cutils/trace.h?q=f:trace%20libcutils

## UI

At the UI level, these functions create slices and counters within the scope of
a process track group, as follows:

![](/docs/images/atrace-slices.png "ATrace slices in the UI")

## SQL

At the SQL level, ATrace events are available in the standard `slice` and
`counter` tables, together with other counters and slices coming from other
data sources.

### Slices

```sql
select s.ts, t.name as thread_name, t.tid, s.name as slice_name, s.dur
from slice as s left join thread_track as trk on s.track_id = trk.id
left join thread as t on trk.utid = t.utid
```

ts | thread_name | tid | slice_name | dur
---|-------------|-----|------------|----
261190068051612 | android.anim | 1317 | dequeueBuffer | 623021
261190068636404 | android.anim | 1317 | importBuffer | 30312
261190068687289 | android.anim | 1317 | lockAsync | 2269428
261190068693852 | android.anim | 1317 | LockBuffer | 2255313
261190068696300 | android.anim | 1317 | MapBuffer | 36302
261190068734529 | android.anim | 1317 | CleanBuffer | 2211198

### Counters

```sql
select ts, p.name as process_name, p.pid, t.name as counter_name, c.value
from counter as c left join process_counter_track as t on c.track_id = t.id
left join process as p on t.upid = p.upid
```

ts | process_name | pid | counter_name | value
---|--------------|-----|--------------|------
261193227069635 | com.android.systemui | 1664 | GPU completion | 0
261193268649379 | com.android.systemui | 1664 | GPU completion | 1
261193269787139 | com.android.systemui | 1664 | HWC release | 1
261193270330890 | com.android.systemui | 1664 | GPU completion | 0
261193271282244 | com.android.systemui | 1664 | GPU completion | 1
261193277112817 | com.android.systemui | 1664 | HWC release | 0

## TraceConfig

```protobuf
buffers {
  size_kb: 102400
  fill_policy: RING_BUFFER
}

data_sources {
  config {
    name: "linux.ftrace"
    ftrace_config {
      # Enables specific system events tags.
      atrace_categories: "am"
      atrace_categories: "pm"

      # Enables events for a specific app.
      atrace_apps: "com.google.android.apps.docs"

      # Enables all events for all apps.
      atrace_apps: "*"
    }
  }
}
```
