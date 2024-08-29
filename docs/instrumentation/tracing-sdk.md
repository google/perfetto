# Tracing SDK

The Perfetto Tracing SDK is a C++17 library that allows userspace applications
to emit trace events and add more app-specific context to a Perfetto trace.

When using the Tracing SDK there are two main aspects to consider:

1. Whether you are interested only in tracing events coming from your own app
   or want to collect full-stack traces that overlay app trace events with
   system trace events like scheduler traces, syscalls or any other Perfetto
   data source.

2. For app-specific tracing, whether you need to trace simple types of timeline
  events (e.g., slices, counters) or need to define complex data sources with a
  custom strongly-typed schema (e.g., for dumping the state of a subsystem of
  your app into the trace).

For Android-only instrumentation, the advice is to keep using the existing
[android.os.Trace (SDK)][atrace-sdk] / [ATrace_* (NDK)][atrace-ndk] if they
are sufficient for your use cases. Atrace-based instrumentation is fully
supported in Perfetto.
See the [Data Sources -> Android System -> Atrace Instrumentation][atrace-ds]
for details.

## Getting started

TIP: The code from these examples is also available [in the
repository](/examples/sdk/README.md).

To start using the Client API, first check out the latest SDK release:

```bash
git clone https://android.googlesource.com/platform/external/perfetto -b v47.0
```

The SDK consists of two files, `sdk/perfetto.h` and `sdk/perfetto.cc`. These are
an amalgamation of the Client API designed to easy to integrate to existing
build systems. The sources are self-contained and require only a C++17 compliant
standard library.

For example, to add the SDK to a CMake project, edit your CMakeLists.txt:

```cmake
cmake_minimum_required(VERSION 3.13)
project(PerfettoExample)
find_package(Threads)

# Define a static library for Perfetto.
include_directories(perfetto/sdk)
add_library(perfetto STATIC perfetto/sdk/perfetto.cc)

# Link the library to your main executable.
add_executable(example example.cc)
target_link_libraries(example perfetto ${CMAKE_THREAD_LIBS_INIT})

if (WIN32)
  # The perfetto library contains many symbols, so it needs the big object
  # format.
  target_compile_options(perfetto PRIVATE "/bigobj")
  # Disable legacy features in windows.h.
  add_definitions(-DWIN32_LEAN_AND_MEAN -DNOMINMAX)
  # On Windows we should link to WinSock2.
  target_link_libraries(example ws2_32)
endif (WIN32)

# Enable standards-compliant mode when using the Visual Studio compiler.
if (MSVC)
  target_compile_options(example PRIVATE "/permissive-")
endif (MSVC)
```

Next, initialize Perfetto in your program:

```C++
#include <perfetto.h>

int main(int argc, char** argv) {
  perfetto::TracingInitArgs args;

  // The backends determine where trace events are recorded. You may select one
  // or more of:

  // 1) The in-process backend only records within the app itself.
  args.backends |= perfetto::kInProcessBackend;

  // 2) The system backend writes events into a system Perfetto daemon,
  //    allowing merging app and system events (e.g., ftrace) on the same
  //    timeline. Requires the Perfetto `traced` daemon to be running (e.g.,
  //    on Android Pie and newer).
  args.backends |= perfetto::kSystemBackend;

  perfetto::Tracing::Initialize(args);
}
```

You are now ready to instrument your app with trace events.

## Custom data sources vs Track events

The SDK offers two abstraction layers to inject tracing data, built on top of
each other, which trade off code complexity vs expressive power:
[track events](#track-events) and [custom data sources](#custom-data-sources).

### Track events

Track events are the suggested option when dealing with app-specific tracing as
they take care of a number of subtleties (e.g., thread safety, flushing, string
interning).
Track events are time bounded events (e.g., slices, counter) based on simple
`TRACE_EVENT` annotation tags in the codebase, like this:

```c++
#include <perfetto.h>

PERFETTO_DEFINE_CATEGORIES(
    perfetto::Category("rendering")
        .SetDescription("Events from the graphics subsystem"),
    perfetto::Category("network")
        .SetDescription("Network upload and download statistics"));

PERFETTO_TRACK_EVENT_STATIC_STORAGE();
...

int main(int argc, char** argv) {
  ...
  perfetto::Tracing::Initialize(args);
  perfetto::TrackEvent::Register();
}

...

void LayerTreeHost::DoUpdateLayers() {
  TRACE_EVENT("rendering", "LayerTreeHost::DoUpdateLayers");
  ...
  for (PictureLayer& pl : layers) {
    TRACE_EVENT("rendering", "PictureLayer::Update");
    pl.Update();
  }
}
```

Which are rendered in the UI as follows:

![Track event example](/docs/images/track-events.png)

Track events are the best default option and serve most tracing use cases with
very little complexity.

To include your new track events in the trace, ensure that the `track_event`
data source is included in the trace config, with a list of enabled and disabled
categories.

```protobuf
data_sources {
  config {
    name: "track_event"
    track_event_config {
        enabled_categories: "rendering"
        disabled_categories: "*"
    }
  }
}
```

See the [Track events page](track-events.md) for full instructions.

### Custom data sources

For most uses, track events are the most straightforward way of instrumenting
apps for tracing. However, in some rare circumstances they are not
flexible enough, e.g., when the data doesn't fit the notion of a track or is
high volume enough that it needs a strongly typed schema to minimize the size of
each event. In this case, you can implement a *custom data source* for
Perfetto.

Unlike track events, when working with custom data sources, you will also need
corresponding changes in [trace processor](/docs/analysis/trace-processor.md)
to enable importing your data format.

A custom data source is a subclass of `perfetto::DataSource`. Perfetto will
automatically create one instance of the class for each tracing session it is
active in (usually just one).

```C++
class CustomDataSource : public perfetto::DataSource<CustomDataSource> {
 public:
  void OnSetup(const SetupArgs&) override {
    // Use this callback to apply any custom configuration to your data source
    // based on the TraceConfig in SetupArgs.
  }

  void OnStart(const StartArgs&) override {
    // This notification can be used to initialize the GPU driver, enable
    // counters, etc. StartArgs will contains the DataSourceDescriptor,
    // which can be extended.
  }

  void OnStop(const StopArgs&) override {
    // Undo any initialization done in OnStart.
  }

  // Data sources can also have per-instance state.
  int my_custom_state = 0;
};

PERFETTO_DECLARE_DATA_SOURCE_STATIC_MEMBERS(CustomDataSource);
```

The data source's static data should be defined in one source file like this:

```C++
PERFETTO_DEFINE_DATA_SOURCE_STATIC_MEMBERS(CustomDataSource);
```

Custom data sources need to be registered with Perfetto:

```C++
int main(int argc, char** argv) {
  ...
  perfetto::Tracing::Initialize(args);
  // Add the following:
  perfetto::DataSourceDescriptor dsd;
  dsd.set_name("com.example.custom_data_source");
  CustomDataSource::Register(dsd);
}
```

As with all data sources, the custom data source needs to be specified in the
trace config to enable tracing:

```C++
perfetto::TraceConfig cfg;
auto* ds_cfg = cfg.add_data_sources()->mutable_config();
ds_cfg->set_name("com.example.custom_data_source");
```

Finally, call the `Trace()` method to record an event with your custom data
source. The lambda function passed to that method will only be called if tracing
is enabled. It is always called synchronously and possibly multiple times if
multiple concurrent tracing sessions are active.

```C++
CustomDataSource::Trace([](CustomDataSource::TraceContext ctx) {
  auto packet = ctx.NewTracePacket();
  packet->set_timestamp(perfetto::TrackEvent::GetTraceTimeNs());
  packet->set_for_testing()->set_str("Hello world!");
});
```

If necessary the `Trace()` method can access the custom data source state
(`my_custom_state` in the example above). Doing so, will take a mutex to
ensure data source isn't destroyed (e.g., because of stopping tracing) while
the `Trace()` method is called on another thread. For example:

```C++
CustomDataSource::Trace([](CustomDataSource::TraceContext ctx) {
  auto safe_handle = trace_args.GetDataSourceLocked();  // Holds a RAII lock.
  DoSomethingWith(safe_handle->my_custom_state);
});
```

## In-process vs System mode

The two modes are not mutually exclusive. An app can be configured to work
in both modes and respond both to in-process tracing requests and system
tracing requests. Both modes generate the same trace file format.

### In-process mode

In this mode both the perfetto service and the app-defined data sources are
hosted fully in-process, in the same process of the profiled app. No connection
to the system `traced` daemon will be attempted.

In-process mode can be enabled by setting
`TracingInitArgs.backends = perfetto::kInProcessBackend` when initializing the
SDK, see examples below.

This mode is used to generate traces that contain only events emitted by
the app, but not other types of events (e.g. scheduler traces).

The main advantage is that by running fully in-process, it doesn't require any
special OS privileges and the profiled process can control the lifecycle of
tracing sessions.

This mode is supported on Android, Linux, MacOS and Windows.

### System mode

In this mode the app-defined data sources will connect to the external `traced`
service using the [IPC over UNIX socket][ipc].

System mode can be enabled by setting
`TracingInitArgs.backends = perfetto::kSystemBackend` when initializing the SDK,
see examples below.

The main advantage of this mode is that it is possible to create fused traces where
app events are overlaid on the same timeline of OS events. This enables
full-stack performance investigations, looking all the way through syscalls and
kernel scheduling events.

The main limitation of this mode is that it requires the external `traced` daemon
to be up and running and reachable through the UNIX socket connection.

This is suggested for local debugging or lab testing scenarios where the user
(or the test harness) can control the OS deployment (e.g., sideload binaries on
Android).

When using system mode, the tracing session must be controlled from the outside,
using the `perfetto` command-line client
(See [reference](/docs/reference/perfetto-cli)). This is because when collecting
system traces, tracing data producers are not allowed to read back the trace
data as it might disclose information about other processes and allow
side-channel attacks.

* On Android 9 (Pie) and beyond, traced is shipped as part of the platform.
* On older versions of Android, traced can be built from sources using the
  the [standalone NDK-based workflow](/docs/contributing/build-instructions.md)
  and sideloaded via adb shell.
* On Linux and MacOS and Windows `traced` must be built and run separately. See
  the [Linux quickstart](/docs/quickstart/linux-tracing.md) for instructions.
* On Windows the tracing protocol works over TCP/IP (
  [127.0.0.1:32278](https://cs.android.com/android/platform/superproject/main/+/main:external/perfetto/src/tracing/ipc/default_socket.cc;l=75;drc=4f88a2fdfd3801c109d5e927b8206f9756288b12)
  ) + named shmem.

## {#recording} Recording traces through the API

_Tracing through the API is currently only supported with the in-process mode.
When using system mode, use the `perfetto` cmdline client (see quickstart
guides)._

First initialize a [TraceConfig](/docs/reference/trace-config-proto.autogen)
message which specifies what type of data to record.

If your app includes [track events](track-events.md) (i.e, `TRACE_EVENT`), you
typically want to choose the categories which are enabled for tracing.

By default, all non-debug categories are enabled, but you can enable a specific
one like this:

```C++
perfetto::protos::gen::TrackEventConfig track_event_cfg;
track_event_cfg.add_disabled_categories("*");
track_event_cfg.add_enabled_categories("rendering");
```

Next, build the main trace config together with the track event part:

```C++
perfetto::TraceConfig cfg;
cfg.add_buffers()->set_size_kb(1024);  // Record up to 1 MiB.
auto* ds_cfg = cfg.add_data_sources()->mutable_config();
ds_cfg->set_name("track_event");
ds_cfg->set_track_event_config_raw(track_event_cfg.SerializeAsString());
```

If your app includes a custom data source, you can also enable it here:

```C++
ds_cfg = cfg.add_data_sources()->mutable_config();
ds_cfg->set_name("my_data_source");
```

After building the trace config, you can begin tracing:

```C++
std::unique_ptr<perfetto::TracingSession> tracing_session(
    perfetto::Tracing::NewTrace());
tracing_session->Setup(cfg);
tracing_session->StartBlocking();
```

TIP: API methods with `Blocking` in their name will suspend the calling thread
     until the respective operation is complete. There are also asynchronous
     variants that don't have this limitation.

Now that tracing is active, instruct your app to perform the operation you
want to record. After that, stop tracing and collect the
protobuf-formatted trace data:

```C++
tracing_session->StopBlocking();
std::vector<char> trace_data(tracing_session->ReadTraceBlocking());

// Write the trace into a file.
std::ofstream output;
output.open("example.perfetto-trace", std::ios::out | std::ios::binary);
output.write(&trace_data[0], trace_data.size());
output.close();
```

To save memory with longer traces, you can also tell Perfetto to write
directly into a file by passing a file descriptor into Setup(), remembering
to close the file after tracing is done:

```C++
int fd = open("example.perfetto-trace", O_RDWR | O_CREAT | O_TRUNC, 0600);
tracing_session->Setup(cfg, fd);
tracing_session->StartBlocking();
// ...
tracing_session->StopBlocking();
close(fd);
```

The resulting trace file can be directly opened in the [Perfetto
UI](https://ui.perfetto.dev) or the [Trace Processor](/docs/analysis/trace-processor.md).

[ipc]: /docs/design-docs/api-and-abi.md#socket-protocol
[atrace-ds]: /docs/data-sources/atrace.md
[atrace-ndk]: https://developer.android.com/ndk/reference/group/tracing
[atrace-sdk]: https://developer.android.com/reference/android/os/Trace
