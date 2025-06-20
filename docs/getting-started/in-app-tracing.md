# Recording In-App Traces with Perfetto

_In this page, you'll learn how to use the Perfetto SDK to collect a trace from
a C++ app which we instrument. You'll view the collected trace with the Perfetto
UI and query its contents programatically with PerfettoSQL._

The Perfetto SDK is a C++ library that allows you to instrument your application
to record trace events. These events can then be visualized and analyzed with
the Perfetto UI and Trace Processor.

## Adding your first instrumentation

### Setup

Checkout the latest SDK release

```
git clone https://github.com/google/perfetto.git -b v50.1
```

The SDK consists of two files, `sdk/perfetto.h` and `sdk/perfetto.cc`. These are
an amalgamation of the Client API designed to easy to integrate to existing
build systems. The sources are self-contained and require only a C++17 compliant
standard library.

Copy them in your project. The next steps assume they're in the `perfetto/sdk`
folder. Assuming your build looks like this:

<?tabs>

TAB: CMake

```
cmake_minimum_required(VERSION 3.13)
project(Example)

# Main executable
add_executable(example example.cc)
```

</tabs?>

You can add the perfetto static library like this:

<?tabs>

TAB: CMake

```
# It's recommended to use a recent version of CMake.
cmake_minimum_required(VERSION 3.13)

# Name of the project.
project(Example)

# Find the thread library, which is a dependency of Perfetto.
find_package(Threads)

# Add the Perfetto SDK source files to a static library.
include_directories(perfetto/sdk)
add_library(perfetto STATIC perfetto/sdk/perfetto.cc)

# Add your application's source files to an executable.
add_executable(example example.cc)

# Link the Perfetto library and the thread library to your executable.
target_link_libraries(example perfetto ${CMAKE_THREAD_LIBS_INIT})

# Windows-specific settings.
if (WIN32)
  # The Perfetto library contains many symbols, so it needs the "big object"
  # format.
  target_compile_options(perfetto PRIVATE "/bigobj")

  # Disable legacy features in windows.h.
  add_definitions(-DWIN32_LEAN_AND_MEAN -DNOMINMAX)

  # On Windows, we need to link to the WinSock2 library.
  target_link_libraries(example ws2_32)
endif (WIN32)

# Enable standards-compliant mode when using the Visual Studio compiler.
if (MSVC)
  target_compile_options(example PRIVATE "/permissive-")
endif (MSVC)
```

</tabs?>

Initialize perfetto in your program and define your tracing categories:

<?tabs>

TAB: C++

```
#include <perfetto.h>

PERFETTO_DEFINE_CATEGORIES(
    perfetto::Category("rendering")
        .SetDescription("Events from the graphics subsystem"),
    perfetto::Category("network")
        .SetDescription("Network upload and download statistics"));

PERFETTO_TRACK_EVENT_STATIC_STORAGE();

int main(int argc, char** argv) {
  perfetto::TracingInitArgs args;
  args.backends |= perfetto::kInProcessBackend;
  perfetto::Tracing::Initialize(args);
  perfetto::TrackEvent::Register();
  //...
}
```

</tabs?>

You can now add instrumentation points to your code. They will emit events when
tracing is enabled.

The `TRACE_EVENT` macro records a scoped event. The event starts when the macro
is called and ends at the end of the current scope (e.g., when the function
returns). This is the most common type of event and is useful for tracing the
duration of a function.

The `TRACE_EVENT_BEGIN` and `TRACE_EVENT_END` macros can be used to record
events that don't follow function scoping. `TRACE_EVENT_BEGIN` starts an event,
and `TRACE_EVENT_END` ends the most recent event started on the same thread by
default, but can be configured to work across threads and even across processes
(see the
[Track Event documentation](/docs/instrumentation/track-events.md#tracks) for
more details). This is useful for tracing operations that span multiple
functions.

The `TRACE_COUNTER` macro records the value of a counter at a specific point in
time. This is useful for tracking things like memory usage or the number of
items in a queue.

<?tabs>

TAB: C++

```
void DrawPlayer(int player_number) {
  TRACE_EVENT("rendering", "DrawPlayer", "player_number", player_number);
  // ...
}

void DrawGame() {
  TRACE_EVENT_BEGIN("rendering", "DrawGame");
  DrawPlayer(1);
  DrawPlayer(2);
  TRACE_EVENT_END("rendering");

  // ...
  TRACE_COUNTER("rendering", "Framerate", 120);
}
```

</tabs?>

## Collecting your first app trace

You can start collecting events with:

<?tabs>

TAB: C++

```
  perfetto::TraceConfig cfg;
  cfg.add_buffers()->set_size_kb(1024);
  auto* ds_cfg = cfg.add_data_sources()->mutable_config();
  ds_cfg->set_name("track_event");
  perfetto::protos::gen::TrackEventConfig te_cfg;
  te_cfg.add_disabled_categories("*");
  te_cfg.add_enabled_categories("rendering");
  ds_cfg->set_track_event_config_raw(te_cfg.SerializeAsString());

  std::unique_ptr<perfetto::TracingSession> tracing_session = perfetto::Tracing::NewTrace();
  tracing_session->Setup(cfg);
  tracing_session->StartBlocking();
  // Keep tracing_session alive

  // ...
```

</tabs?>

And you can stop and save them into a file with:

<?tabs>

TAB: C++

```
  tracing_session->StopBlocking();
  std::vector<char> trace_data(tracing_session->ReadTraceBlocking());

  // Write the result into a file.
  std::ofstream output;
  output.open("example.pftrace", std::ios::out | std::ios::binary);
  output.write(&trace_data[0], std::streamsize(trace_data.size()));
  output.close();
```

</tabs?>

## Visualizing your first app trace

You can now open the `example.pftrace` file with https://ui.perfetto.dev/

It will show the events captured by the execution of your instrumentation
points:

![Track event example](/docs/images/track_event_draw_game.png)

## Analysing your first app trace

You can also write SQL queries to understand more about the trace.

By going into the `Query (SQL)` pane and pasting the following query:

```
SELECT
  dur AS duration_ns,
  EXTRACT_ARG(slice.arg_set_id, 'debug.player_number') AS player_number
FROM slice
WHERE slice.name = 'DrawPlayer';
```

you can see how many times the `DrawPlayer` instrumentation point has been hit,
how long each execution took and its `player_number` annotation.

![SQL query example](/docs/images/sql_draw_player.png)

## Combined in-app and system tracing

If you want to inspect your app events alongside system events, you can:

- Change your app code to connect to the perfetto central tracing service
  instead of logging inside the process.

<?tabs>

TAB: C++

```
  perfetto::TracingInitArgs args;
  args.backends |= perfetto::kSystemBackend;
  perfetto::Tracing::Initialize(args);
```

</tabs?>

- Remove the code start and stop collecting events from your app. You can start
  and stop collecting event using the
  [perfetto command line client](/docs/reference/perfetto-cli).

- Learn more about [system tracing](/docs/getting-started/system-tracing.md)

## Next steps

Now that you've recorded and analyzed your first in-app trace, you can explore
more advanced topics:

- **Learn more about the Perfetto SDK:** The
  [Perfetto SDK documentation](/docs/instrumentation/tracing-sdk.md) provides
  more details on how to use the SDK, including how to define custom data
  sources and use different types of tracks.
- **Explore other data sources:** Perfetto supports a wide range of data
  sources that you can use to collect more information about your application
  and the system it's running on. For example, you can collect
  [CPU scheduling events](/docs/data-sources/cpu-scheduling.md),
  [memory usage information](/docs/data-sources/memory-counters.md), or
  [Android-specific events](/docs/data-sources/atrace.md).
