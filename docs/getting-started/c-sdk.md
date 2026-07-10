# Recording In-App Traces with the C SDK

In this guide, you'll learn how to:

- Use the Perfetto C SDK to add custom trace points to a C application.
- Record a trace containing your custom events, fully in-process.
- Visualize the trace in the Perfetto UI.
- Programmatically analyze the trace using PerfettoSQL.

The Perfetto **C SDK** is the ABI-stable foundation that all of Perfetto's
language bindings are built on. Use it when you need a stable-by-design ABI, a
shared-library boundary, no C++17 dependency, or an FFI target for another
language. If you are writing a plain C++ application, the
[C++ SDK](/docs/instrumentation/tracing-sdk.md) is more ergonomic — see
[Choosing a Perfetto SDK](/docs/instrumentation/choosing-an-sdk.md) to decide.

WARNING: The C SDK is not yet stable — its API and ABI are subject to change.
See [ABI stability](/docs/reference/c-sdk-api.md#stability) before depending on
it.

TIP: The complete, runnable code for this guide is in the repository at
[`examples/shared_lib/example_shlib_in_process.c`](https://github.com/google/perfetto/blob/main/examples/shared_lib/example_shlib_in_process.c).

## Setup

The C SDK is distributed as the `libperfetto_c` shared library plus the public
headers under `include/perfetto/public/`. Check out Perfetto and build the
library:

```bash
git clone https://github.com/google/perfetto.git
cd perfetto
tools/install-build-deps
tools/gn gen out/linux
tools/ninja -C out/linux libperfetto_c
```

This produces `out/linux/libperfetto_c.so`. To build an application against it,
point your compiler at the checkout's `include/` directory for the headers and
at that shared library to link.

Save the complete example for this guide —
[`example_shlib_in_process.c`](https://github.com/google/perfetto/blob/main/examples/shared_lib/example_shlib_in_process.c),
the code assembled across the sections below — locally as `example.c`, then build
it with whichever toolchain fits your project:

<?tabs>

TAB: Command line

```bash
# Point these at your Perfetto checkout and its GN output directory.
PERFETTO_ROOT=/path/to/perfetto
PERFETTO_OUT="$PERFETTO_ROOT/out/linux"

cc example.c -I"$PERFETTO_ROOT/include" -L"$PERFETTO_OUT" -lperfetto_c -o example
```

TAB: CMake

```cmake
cmake_minimum_required(VERSION 3.13)
project(Example C)

# Point these at your Perfetto checkout and its GN output directory.
set(PERFETTO_ROOT /path/to/perfetto)
set(PERFETTO_OUT ${PERFETTO_ROOT}/out/linux)

add_executable(example example.c)
target_include_directories(example PRIVATE ${PERFETTO_ROOT}/include)

find_library(PERFETTO_C_LIB perfetto_c PATHS ${PERFETTO_OUT} REQUIRED)
target_link_libraries(example ${PERFETTO_C_LIB})
```

</tabs?>

NOTE: The C SDK requires C11 or later (for atomics), built with the compiler's
**default GNU dialect** — which is what you get out of the box, so no `-std` flag
is needed. Do not force strict `-std=c11`: it disables the POSIX extensions the
headers rely on (e.g. `syscall()`), and the build will fail. If you must set the
standard explicitly, use `-std=gnu11` (or add `-D_GNU_SOURCE`). On Windows, C11
atomics are not well supported, so the C SDK targets Linux, macOS and Android.

`libperfetto_c` is a *shared* library, so the dynamic loader must find it at run
time. Because it lives in the GN output directory rather than a system path,
point `LD_LIBRARY_PATH` at it when you run the program (on macOS, use
`DYLD_LIBRARY_PATH`):

```bash
LD_LIBRARY_PATH="$PERFETTO_OUT" ./example
```

On success the program writes `example.pftrace` to the current directory and
prints:

```text
Wrote example.pftrace
```

NOTE: The in-process tracing service also logs informational messages to stderr
while the trace runs — lines such as `Configured tracing session 1, ...` and
`Producer connected`. These are normal and safe to ignore; the only real output
is the `Wrote example.pftrace` line above. (A debug build of the library — the
default from `tools/gn gen out/linux` — is the most verbose; building with
`tools/gn gen --args='is_debug=false' out/linux` drops the debug-only lines.)

## Adding your first instrumentation

Initialize Perfetto and declare your tracing categories. Categories are declared
with an X-macro list so they can be defined once and registered together:

```c
#include "perfetto/public/producer.h"
#include "perfetto/public/te_category_macros.h"
#include "perfetto/public/te_macros.h"
#include "perfetto/public/track_event.h"

// Declare the categories used by this program.
#define EXAMPLE_CATEGORIES(C) \
  C(rendering, "rendering", "Rendering events")

PERFETTO_TE_CATEGORIES_DEFINE(EXAMPLE_CATEGORIES)

int main(void) {
  struct PerfettoProducerInitArgs args = PERFETTO_PRODUCER_INIT_ARGS_INIT();
  args.backends = PERFETTO_BACKEND_IN_PROCESS;
  PerfettoProducerInit(args);

  PerfettoTeInit();
  PERFETTO_TE_REGISTER_CATEGORIES(EXAMPLE_CATEGORIES);
  // ...
}
```

You can now add instrumentation points. They emit events only when tracing is
enabled for their category. The main building block is the `PERFETTO_TE` macro,
whose first argument is a category and whose second argument is the event type:

- `PERFETTO_TE_SLICE_BEGIN("name")` / `PERFETTO_TE_SLICE_END()` record a
  **slice** — a duration on a track. Slices nest.
- `PERFETTO_TE_INSTANT("name")` records an **instant** — a single point in time.
- `PERFETTO_TE_COUNTER()` records a **counter** value (see
  [Track Events](/docs/instrumentation/c-sdk-track-events.md)).

Additional arguments annotate the event. For example, `PERFETTO_TE_ARG_INT64`
attaches a debug annotation:

```c
static void DrawPlayer(int player_number) {
  PERFETTO_TE(rendering, PERFETTO_TE_SLICE_BEGIN("DrawPlayer"),
              PERFETTO_TE_ARG_INT64("player_number", player_number));
  // ... draw the player ...
  PERFETTO_TE(rendering, PERFETTO_TE_SLICE_END());
}

static void DrawGame(void) {
  PERFETTO_TE(rendering, PERFETTO_TE_SLICE_BEGIN("DrawGame"));
  DrawPlayer(1);
  DrawPlayer(2);
  PERFETTO_TE(rendering, PERFETTO_TE_SLICE_END());
}
```

## Collecting your first app trace

With the in-process backend, your program is both the producer of events and the
controller of the tracing session. Recording has three steps: build a
`TraceConfig`, start a session, and read the result back out.

The `TraceConfig` is a protobuf message. The C SDK builds it with the protozero
serialization helpers and the generated `.pzc.h` accessors. This is more verbose
than the C++ SDK's typed builders, but requires no C++ runtime:

```c
#include "perfetto/public/abi/heap_buffer.h"
#include "perfetto/public/pb_msg.h"
#include "perfetto/public/protos/config/data_source_config.pzc.h"
#include "perfetto/public/protos/config/trace_config.pzc.h"
#include "perfetto/public/protos/config/track_event/track_event_config.pzc.h"
#include "perfetto/public/stream_writer.h"
#include "perfetto/public/tracing_session.h"

#include <stdio.h>   // fopen, fwrite, fclose
#include <stdlib.h>  // malloc, free
#include <string.h>  // strlen

// Returns a malloc'd serialized TraceConfig; caller frees it.
static void* BuildTraceConfig(size_t* size) {
  struct PerfettoPbMsgWriter writer;
  struct PerfettoHeapBuffer* hb = PerfettoHeapBufferCreate(&writer.writer);
  struct perfetto_protos_TraceConfig cfg;
  PerfettoPbMsgInit(&cfg.msg, &writer);

  // A 1 MiB in-memory buffer.
  struct perfetto_protos_TraceConfig_BufferConfig buffers;
  perfetto_protos_TraceConfig_begin_buffers(&cfg, &buffers);
  perfetto_protos_TraceConfig_BufferConfig_set_size_kb(&buffers, 1024);
  perfetto_protos_TraceConfig_end_buffers(&cfg, &buffers);

  // Enable the "track_event" data source with the "rendering" category.
  struct perfetto_protos_TraceConfig_DataSource data_sources;
  perfetto_protos_TraceConfig_begin_data_sources(&cfg, &data_sources);
  struct perfetto_protos_DataSourceConfig ds_cfg;
  perfetto_protos_TraceConfig_DataSource_begin_config(&data_sources, &ds_cfg);
  perfetto_protos_DataSourceConfig_set_cstr_name(&ds_cfg, "track_event");
  struct perfetto_protos_TrackEventConfig te_cfg;
  perfetto_protos_DataSourceConfig_begin_track_event_config(&ds_cfg, &te_cfg);
  perfetto_protos_TrackEventConfig_set_enabled_categories(&te_cfg, "rendering",
                                                          strlen("rendering"));
  perfetto_protos_DataSourceConfig_end_track_event_config(&ds_cfg, &te_cfg);
  perfetto_protos_TraceConfig_DataSource_end_config(&data_sources, &ds_cfg);
  perfetto_protos_TraceConfig_end_data_sources(&cfg, &data_sources);

  size_t sz = PerfettoStreamWriterGetWrittenSize(&writer.writer);
  void* buf = malloc(sz);
  PerfettoHeapBufferCopyInto(hb, &writer.writer, buf, sz);
  PerfettoHeapBufferDestroy(hb, &writer.writer);
  *size = sz;
  return buf;
}
```

NOTE: The example above is written for readability. To match the Perfetto tree's
C style, the
[repository version](https://github.com/google/perfetto/blob/main/examples/shared_lib/example_shlib_in_process.c)
hoists its declarations to the top of each scope. Both compile to the same thing.

Then set up the session, start recording, and run your workload:

```c
size_t cfg_size = 0;
void* cfg = BuildTraceConfig(&cfg_size);

struct PerfettoTracingSessionImpl* session =
    PerfettoTracingSessionCreate(PERFETTO_BACKEND_IN_PROCESS);
PerfettoTracingSessionSetup(session, cfg, cfg_size);
free(cfg);
PerfettoTracingSessionStartBlocking(session);

DrawGame();  // Your instrumented workload.
```

TIP: Functions with `Blocking` in their name suspend the calling thread until
the operation completes. Asynchronous variants (`...Async`) are also available.

Finally, stop tracing and write the buffered data to a file. The read API
delivers the trace in chunks through a callback:

```c
static void ReadTraceCb(struct PerfettoTracingSessionImpl* session,
                        const void* data, size_t size, bool has_more,
                        void* user_arg) {
  fwrite(data, 1, size, (FILE*)user_arg);
}

// ...
PerfettoTracingSessionStopBlocking(session);
FILE* f = fopen("example.pftrace", "wb");
PerfettoTracingSessionReadTraceBlocking(session, ReadTraceCb, f);
fclose(f);
PerfettoTracingSessionDestroy(session);
```

## Visualizing your first app trace

Open the `example.pftrace` file with https://ui.perfetto.dev/. You will see the
`DrawGame` and `DrawPlayer` slices captured by your instrumentation:

![Track event example](/docs/images/track_event_draw_game.png)

## Querying your first app trace

As well as visualizing traces, Perfetto can query them with SQL. In the Perfetto
UI, open the "Query (SQL)" tab:

![Perfetto UI Query SQL](/docs/images/perfetto-ui-query-sql.png)

Run a query with Ctrl/Cmd + Enter. For example, to see each `DrawPlayer`
invocation, how long it took, and its `player_number` annotation:

```sql
SELECT
  dur AS duration_ns,
  EXTRACT_ARG(slice.arg_set_id, 'debug.player_number') AS player_number
FROM slice
WHERE slice.name = 'DrawPlayer';
```

![SQL query example](/docs/images/sql_draw_player.png)

## Combined in-app and system tracing

In-app tracing shows your application in isolation. Its real power comes from
combining it with a system-wide trace, so your events line up with CPU
scheduling, memory and I/O.

To do that, initialize the SDK with the **system backend** instead of the
in-process backend:

```c
struct PerfettoProducerInitArgs args = PERFETTO_PRODUCER_INIT_ARGS_INIT();
args.backends = PERFETTO_BACKEND_SYSTEM;
PerfettoProducerInit(args);
PerfettoTeInit();
PERFETTO_TE_REGISTER_CATEGORIES(EXAMPLE_CATEGORIES);
```

Your application now only *produces* events; the external `traced` service
controls when tracing starts and stops. You no longer create a
`PerfettoTracingSession` in-process — instead, record a trace with the standard
[system tracing](/docs/getting-started/system-tracing.md) tools, enabling the
`track_event` data source alongside any system data sources (e.g.
`linux.ftrace`). Your app's custom tracks appear on the same timeline as the
system tracks. See
[`examples/shared_lib/example_shlib_track_event.c`](https://github.com/google/perfetto/blob/main/examples/shared_lib/example_shlib_track_event.c)
for a system-backend example.

## Next steps

- **[Track Events](/docs/instrumentation/c-sdk-track-events.md)**: the full range
  of event types — counters, flows, custom tracks, and more.
- **[Custom Data Sources](/docs/instrumentation/c-sdk-data-sources.md)**: emit
  strongly-typed, high-volume data with your own protobuf schema.
- **[C SDK Reference](/docs/reference/c-sdk-api.md)**: headers, functions, and the
  stability contract.
