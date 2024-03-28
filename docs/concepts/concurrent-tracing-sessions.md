# Concurrent tracing sessions

Perfetto supports multiple concurrent tracing sessions.
Sessions are isolated from each other each and each session can choose a different mix of producers and data sources in its [config](config.md) and, in general, it will only receive events specified by that config.
This is a powerful mechanism which allows great flexibility when collecting traces from the lab or field.
However there are a few caveats to bear in mind with concurrent tracing sessions:
1. [Some data sources do not support concurrent sessions](#some-data-sources-do-not-support-concurrent-sessions)
2. [Some settings are per session while others are per producer](#some-settings-are-per-session-while-others-are-per-producer)
3. Due to the [way atrace works works](#atrace) if a session requests *any* atrace category or app it receives *all* atrace events enabled on the device
4. [Various limits](#various-limits) apply

## Some data sources do not support concurrent sessions

Whilst most data sources implemented with the Perfetto SDK as well as most data sources provided by the Perfetto team, do support concurrent tracing sessions some do not.
This can be due to:

- Hardware or driver constraints
- Difficulty of implementing the config muxing
- Perfetto SDK: users may [opt-out of multiple session support](https://cs.android.com/android/platform/superproject/main/+/main:external/perfetto/include/perfetto/tracing/data_source.h;l=266;drc=f988c792c18f93841b14ffa71019fdedf7ab2f03)

### Known to work
- `traced_probes` data sources ([linux.ftrace](/docs/reference/trace-config-proto.autogen#FtraceConfig), [linux.process_stats](/docs/reference/trace-config-proto.autogen#ProcessStatsConfig), [linux.sys_stats](/docs/reference/trace-config-proto.autogen#SysStatsConfig), [linux.system_info](https://perfetto.dev/docs/reference/trace-config-proto.autogen#SystemInfoConfig), etc)

### Known to work with caveats
- `heapprofd` supports multiple sessions but each process can only be in one session.
- `traced_perf` in general supports multiple sessions but the kernel has a limit on counters so may reject a config.

### Known not to work
- `traced metatracing`

## Some settings are per session while others are per producer

Most buffer sizes and timings specified in the config are per session.
For example the buffer [sizes](https://cs.android.com/android/platform/superproject/main/+/main:external/perfetto/protos/perfetto/config/trace_config.proto;l=32?q=f:perfetto%20f:trace_config&ss=android%2Fplatform%2Fsuperproject%2Fmain).

However some parameters configure per-producer settings: for example the [size and layout](https://cs.android.com/android/platform/superproject/main/+/main:external/perfetto/protos/perfetto/config/trace_config.proto;l=182;drc=488df1649781de42b72e981c5e79ad922508d1e5) of the shmem buffer between the producer and traced.
While that is general data source setting the same can apply to data source specific settings.
For example the ftrace [kernel buffer size and drain period](https://cs.android.com/android/platform/superproject/main/+/main:external/perfetto/protos/perfetto/config/ftrace/ftrace_config.proto;l=32;drc=6a3d3540e68f3d5949b5d86ca736bfd7f811deff) are settings that have to be shared between all users of `traced_probes`.

Bear in mind that
- Some resources like the shmem buffers are shared by all sessions
- As suggested by the comments in linked code above some settings are best treated as 'hints' since another config may have already set them before you get a chance to.

## Atrace

Atrace is an Android specific mechanism for doing userland instrumentation and the only available tracing method prior to the introduction of the Perfetto SDK into Android.
It still powers [os.Trace](https://developer.android.com/reference/android/os/Trace) (as used by platform and application Java code) and [ATRACE_*](https://cs.android.com/android/platform/superproject/main/+/main:system/core/libcutils/include/cutils/trace.h;l=188;drc=0c44d8d68d56c7aecb828d8d87fba7dcb114f3d9) (as used by platform C++).


Atrace (both prior to Perfetto and via Perfetto) works as follows:
- Configuration:
  - Users choose zero or more 'categories' from a hardcoded list
  - Users choose zero or more package names including globs
- This sets:
  - Some kernel ftrace events
  - A [system property bitmask](https://cs.android.com/android/platform/superproject/main/+/main:frameworks/native/cmds/atrace/atrace.cpp;l=306;drc=c8af4d3407f3d6be46fafdfc044ace55944fb4b7) (for the atrace categories)
  - A [system property](https://cs.android.com/android/platform/superproject/main/+/main:frameworks/native/cmds/atrace/atrace.cpp;l=306;bpv=1;bpt=1) for each package.
- When the Java or C++ tracing APIs are called we examine the system props.
- If the relevant category or package is enabled we write the event to `trace_marker`

As mentioned, each category may enable a number of kernel ftrace events.
For example the 'sched' atrace category enables the `sched/sched_switch` ftrace event.
Kernel ftrace events do not suffer from the current session issues so will not be described further.

For the userland instrumentation:
- Perfetto ensures the union of all atrace packages categories are installed
- However since:
  - the atrace system properties are global
  - we cannot tell which event comes from which category/package
Every session that requests *any* atrace event gets *all* enabled atrace events.

## Various limits
- Perfetto SDK: Max 8 datasource instances per datasource type per producer
- `traced`: Limit of [15 concurrent sessions](https://cs.android.com/android/platform/superproject/main/+/main:external/perfetto/src/tracing/service/tracing_service_impl.cc;l=114?q=kMaxConcurrentTracingSessions%20)
- `traced`: Limit of [5 (10 for statsd) concurrent sessions per UID](https://cs.android.com/android/platform/superproject/main/+/main:external/perfetto/src/tracing/service/tracing_service_impl.cc;l=115;drc=17d5806d458e214bdb829deeeb08b098c2b5254d)

