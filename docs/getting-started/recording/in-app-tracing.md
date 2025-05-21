# Recording In-App Traces with Perfetto

*In this page, you'll learn how to use the Perfetto SDK collect a trace from a
C++ app which we instrument. You'll view the collected trace with the Perfetto
UI and query its contents programatically with PerfettoSQL.*

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

Copy them in your project. The next steps assume they're in the `perfetto/sdk` folder.
Assuming your build looks like this:

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
cmake_minimum_required(VERSION 3.13)
project(Example)
find_package(Threads)

# Define a static library for Perfetto.
include_directories(perfetto/sdk)
add_library(perfetto STATIC perfetto/sdk/perfetto.cc)

# Main executable
add_executable(example example.cc)

# Link the library to your main executable.
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

Example of query in the UI

## Next steps

* SDK: more instrumentation points.
* SDK: system mode.
* SDK: custom data sources.
* Trace analysis: SQL
