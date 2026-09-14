# Concurrent tracing sessions

Perfetto supports multiple concurrent tracing sessions. Sessions are isolated
from each other. Each session can choose a different mix of producers and data
sources in its [config](config.md) and, in general, receives only the events
specified by that config.

Concurrent tracing sessions have a few caveats:

1. [Some data sources do not support concurrent sessions](#some-data-sources-do-not-support-concurrent-sessions)
2. [Some settings are per session while others are per producer](#some-settings-are-per-session-while-others-are-per-producer)
3. Due to the [way atrace works](#atrace), if a session requests *any* atrace
   category or app, it receives *all* atrace events enabled on the device
4. [Various limits](#various-limits) apply

## Some data sources do not support concurrent sessions

Most data sources implemented with the Perfetto SDK or provided by the Perfetto
team support concurrent tracing sessions. Some do not, for these reasons:

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

Most buffer sizes and timings specified in the config are per session,
including buffer
[sizes](https://cs.android.com/android/platform/superproject/main/+/main:external/perfetto/protos/perfetto/config/trace_config.proto;l=32?q=f:perfetto%20f:trace_config&ss=android%2Fplatform%2Fsuperproject%2Fmain).

Some parameters apply per producer, such as the
[size and layout](https://cs.android.com/android/platform/superproject/main/+/main:external/perfetto/protos/perfetto/config/trace_config.proto;l=182;drc=488df1649781de42b72e981c5e79ad922508d1e5)
of the shmem buffer between the producer and traced.
This also applies to settings specific to a data source.
For example, the ftrace
[kernel buffer size and drain period](https://cs.android.com/android/platform/superproject/main/+/main:external/perfetto/protos/perfetto/config/ftrace/ftrace_config.proto;l=32;drc=6a3d3540e68f3d5949b5d86ca736bfd7f811deff)
are settings that have to be shared between all users of `traced_probes`.

Keep in mind:

- Some resources, such as the shmem buffers, are shared by all sessions.
- As the comments in the linked code suggest, some settings are best treated as
  'hints' because another config may have already set them.

## Atrace

Atrace is an Android-specific mechanism for userland instrumentation. It was
the only available tracing method before the Perfetto SDK was introduced into
Android.
It still powers [os.Trace](https://developer.android.com/reference/android/os/Trace) (as used by platform and application Java code) and [ATRACE_*](https://cs.android.com/android/platform/superproject/main/+/main:system/core/libcutils/include/cutils/trace.h;l=188;drc=0c44d8d68d56c7aecb828d8d87fba7dcb114f3d9) (as used by platform C++).


Atrace (both prior to Perfetto and via Perfetto) works as follows:
- Configuration:
  - Users choose zero or more 'categories' from a hardcoded list
  - Users choose zero or more package names including globs
- This sets:
  - Some kernel ftrace events
  - A [system property bitmask](https://cs.android.com/android/platform/superproject/main/+/main:frameworks/native/cmds/atrace/atrace.cpp;l=306;drc=c8af4d3407f3d6be46fafdfc044ace55944fb4b7) (for the atrace categories)
  - A [system property](https://cs.android.com/android/platform/superproject/main/+/main:frameworks/native/cmds/atrace/atrace.cpp;l=306;bpv=1;bpt=1) for each package.
- When the Java or C++ tracing APIs are called, they examine the system
  properties.
- If the relevant category or package is enabled, they write the event to
  `trace_marker`.

Each category may enable several kernel ftrace events. For example, the 'sched'
atrace category enables the `sched/sched_switch` ftrace event. Kernel ftrace
events do not have the concurrent-session issues described below.

For userland instrumentation, Perfetto enables the union of all requested atrace
packages and categories. Every session that requests *any* atrace event gets
*all* enabled atrace events because:

- The atrace system properties are global.
- Perfetto cannot tell which event comes from which category or package.

## Various limits
- Perfetto SDK: Max 8 datasource instances per datasource type per producer
- `traced`: Limit of [15 concurrent sessions](https://cs.android.com/android/platform/superproject/main/+/main:external/perfetto/src/tracing/service/tracing_service_impl.cc;l=114?q=kMaxConcurrentTracingSessions%20)
- `traced`: Limit of [5 (10 for statsd) concurrent sessions per UID](https://cs.android.com/android/platform/superproject/main/+/main:external/perfetto/src/tracing/service/tracing_service_impl.cc;l=115;drc=17d5806d458e214bdb829deeeb08b098c2b5254d)

