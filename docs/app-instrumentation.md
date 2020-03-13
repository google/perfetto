# App instrumentation

The Perfetto Client API is a C++ library that allows applications to emit
trace events to add more context to a Perfetto trace to help with
development, debugging and performance analysis.

> The code from this example is also available as a [GitHub repository](
> https://github.com/skyostil/perfetto-sdk-example).

To start using the Client API, first check out the latest SDK release:

```sh
$ git clone https://android.googlesource.com/platform/external/perfetto -b latest
```

The SDK consists of two files, `sdk/perfetto.h` and
`sdk/perfetto.cc`. These are an amalgamation of the Client API designed to
easy to integrate to existing build systems. For example, to add the SDK to a
CMake project, edit your `CMakeLists.txt` accordingly:

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
```

Next, initialize Perfetto in your program:

```C++

#include <perfetto.h>

int main(int argv, char** argc) {
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

You are now ready to instrument your app with trace events. The Client API
has two options for this:

- [Track events](#track-events), which represent time-bounded operations
   (e.g., function calls) on a timeline. Track events are a good choice for
   most apps.

- [Custom data sources](#custom-data-sources), which can be used to
   efficiently record arbitrary app-defined data using a protobuf encoding.
   Custom data sources are a typically better match for advanced Perfetto
   users.

# Track events

![Track events shown in the Perfetto UI](
  track-events.png "Track events in the Perfetto UI")

*Track events* are application specific, time bounded events recorded into a
*trace* while the application is running. Track events are always associated
with a *track*, which is a timeline of monotonically increasing time. A track
corresponds to an independent sequence of execution, such as a single thread
in a process.

There are a few main types of track events:

1. **Slices**, which represent nested, time bounded operations. For example,
  a slice could cover the time period from when a function begins executing
  to when it returns, the time spent loading a file from the network or the
  time spent blocked on a disk read.

2. **Counters**, which are snapshots of time-varying numeric values. For
  example, a track event can record instantaneous the memory usage of a
  process during its execution.

3. **Flows**, which are used to connect related slices that span different
  tracks together. For example, if an image file is first loaded from
  the network and then decoded on a thread pool, a flow event can be used to
  highlight its path through the system. (Not fully implemented yet).

The [Perfetto UI](https://ui.perfetto.dev) has built in support for track
events, which provides a useful way to quickly visualize the internal
processing of an app. For example, the [Chrome
browser](https://www.chromium.org/developers/how-tos/trace-event-profiling-tool)
is deeply instrumented with track events to assist in debugging, development
and performance analysis.

A typical use case for track events is annotating a function with a scoped
track event, so that function's execution shows up in a trace. To start using
track events, first define the set of categories that your events will fall
into. Each category can be separately enabled or disabled for tracing.

Add the list of categories into a header file (e.g., `example_tracing.h`)
like this:

```C++
#include <perfetto.h>

PERFETTO_DEFINE_CATEGORIES(
    perfetto::Category("rendering")
        .SetDescription("Events from the graphics subsystem"),
    perfetto::Category("network")
        .SetDescription("Network upload and download statistics"));
```

Then, declare static storage for the categories in a cc file (e.g.,
`example_tracing.cc`):

```C++
#include "example_tracing.h"

PERFETTO_TRACK_EVENT_STATIC_STORAGE();
```

Finally, initialize track events after the client library is brought up:

```C++
int main(int argv, char** argc) {
  ...
  perfetto::Tracing::Initialize(args);
  perfetto::TrackEvent::Register();  // Add this.
}
```

Now you can add track events to existing functions like this:

```C++
#include "example_tracing.h"

void DrawPlayer() {
  TRACE_EVENT("rendering", "DrawPlayer");
  ...
}
```

This type of trace event is scoped, which means it will cover the time from
when the function began executing until the point of return. You can also
supply (up to two) debug annotations together with the event.

```C++
int player_number = 1;
TRACE_EVENT("rendering", "DrawPlayer", "player_number", player_number);
```

For more complex arguments, you can define [your own protobuf
messages](../protos/perfetto/trace/track_event/track_event.proto) and emit
them as a parameter for the event.

> Currently custom protobuf messages need to be added directly to the
> Perfetto repository under `protos/perfetto/trace`, and Perfetto itself must
> also be rebuilt. We are working [to lift this
> limitation](https://github.com/google/perfetto/issues/11).

As an example of a custom track event argument type, save the following as
`protos/perfetto/trace/track_event/player_info.proto`:

```protobuf
message PlayerInfo {
  optional string name = 1;
  optional uint64 score = 2;
}
```

This new file should also be added to
`protos/perfetto/trace/track_event/BUILD.gn`:

```json
sources = [
  ...
  "player_info.proto"
]
```

Also, a matching argument should be added to the track event message
definition in
`protos/perfetto/trace/track_event/track_event.proto`:

```protobuf
import "protos/perfetto/trace/track_event/player_info.proto";

...

message TrackEvent {
  ...
  // New argument types go here :)
  optional PlayerInfo player_info = 1000;
}
```

The corresponding trace point could look like this:

```C++
Player my_player;
TRACE_EVENT("category", "MyEvent", [&](perfetto::EventContext ctx) {
  auto player = ctx.event()->set_player_info();
  player->set_name(my_player.name());
  player->set_player_score(my_player.score());
});
```

Now that you have instrumented your app with track events, you are ready to
start [recording traces](recording-traces.md).

# Custom data sources

TODO(skyostil).

## Category configuration

TODO(skyostil).

# Advanced topics

## Tracks

TODO(skyostil).

## Interning

TODO(skyostil).

## Counters

TODO(skyostil).

## Flow events

TODO(skyostil).
