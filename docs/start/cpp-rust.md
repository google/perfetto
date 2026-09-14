# Perfetto for C, C++ and Rust developers

The Perfetto Tracing SDKs add trace points to your own code. Record them from
inside your app, or connect to the system tracing service so your events sit
next to CPU scheduling and other system data. The SDKs work on Linux, Android,
macOS and Windows.

There is a C++ SDK, a C SDK and a community-maintained Rust SDK built on the C
SDK.

## What you can do

<div class="docs-home">
<div class="home-cards home-pair">
<section class="home-card"><h3><span class="home-icon material-icons-round role-record" aria-hidden="true">code</span>Instrument C++</h3><p>Add two source files to your build, then record slices and counters with <code>TRACE_EVENT</code> and <code>TRACE_COUNTER</code>.</p><ul><li><a href="/docs/getting-started/in-app-tracing">Instrument a C++ app</a></li><li><a href="/docs/instrumentation/track-events">Track events (C++ SDK)</a></li></ul></section>
<section class="home-card"><h3><span class="home-icon material-icons-round role-record" aria-hidden="true">code</span>Instrument Rust</h3><p>Add the <code>perfetto-sdk</code> crate, trace whole functions with <code>#[tracefn]</code> from <code>perfetto-sdk-derive</code>, or use <code>tracing-perfetto-sdk</code> as a layer for the <code>tracing</code> crate.</p><ul><li><a href="/docs/getting-started/rust-sdk">Instrument a Rust app</a></li></ul></section>
<section class="home-card"><h3><span class="home-icon material-icons-round role-record" aria-hidden="true">merge</span>Combine with system traces</h3><p>Use the system backend to record your events in the same trace as scheduling, CPU frequency and other system data.</p><ul><li><a href="/docs/getting-started/system-tracing">Record your first system trace</a></li><li><a href="/docs/concepts/service-model">How Perfetto works</a></li></ul></section>
<section class="home-card"><h3><span class="home-icon material-icons-round role-record" aria-hidden="true">extension</span>Extend the SDK</h3><p>Attach your own typed fields to events, send trace data elsewhere with interceptors, and profile custom allocators on Android.</p><ul><li><a href="/docs/instrumentation/extensions">Add custom fields with proto extensions</a></li><li><a href="/docs/instrumentation/heapprofd-api">Profile a custom allocator</a></li></ul></section>
</div>
</div>

## Try it in five minutes

This records a short C++ trace from inside a program. For Rust, see
[Instrument a Rust app](/docs/getting-started/rust-sdk.md).

1.  Download `perfetto-cpp-sdk-src.zip` from the
    [latest Perfetto release](https://github.com/google/perfetto/releases/latest)
    and put `perfetto.h` and `perfetto.cc` in a new directory.

2.  Next to them, create `main.cc`:

    ```cpp
    #include <fstream>
    #include <vector>

    #include "perfetto.h"

    PERFETTO_DEFINE_CATEGORIES(
        perfetto::Category("rendering")
            .SetDescription("Events from the graphics subsystem"));

    PERFETTO_TRACK_EVENT_STATIC_STORAGE();

    void DrawPlayer(int player_number) {
      TRACE_EVENT("rendering", "DrawPlayer", "player_number", player_number);
    }

    int main() {
      perfetto::TracingInitArgs args;
      args.backends |= perfetto::kInProcessBackend;
      perfetto::Tracing::Initialize(args);
      perfetto::TrackEvent::Register();

      // Record the "rendering" category into a 1 MB in-memory buffer.
      perfetto::TraceConfig cfg;
      cfg.add_buffers()->set_size_kb(1024);
      auto* ds_cfg = cfg.add_data_sources()->mutable_config();
      ds_cfg->set_name("track_event");
      perfetto::protos::gen::TrackEventConfig te_cfg;
      te_cfg.add_enabled_categories("rendering");
      ds_cfg->set_track_event_config_raw(te_cfg.SerializeAsString());

      auto session = perfetto::Tracing::NewTrace();
      session->Setup(cfg);
      session->StartBlocking();

      for (int frame = 0; frame < 3; frame++) {
        TRACE_EVENT("rendering", "DrawFrame");
        DrawPlayer(1);
        DrawPlayer(2);
        TRACE_COUNTER("rendering", "Framerate", 120);
      }

      session->StopBlocking();
      std::vector<char> trace(session->ReadTraceBlocking());
      std::ofstream out("example.pftrace", std::ios::binary);
      out.write(trace.data(), static_cast<std::streamsize>(trace.size()));
    }
    ```

3.  Build and run it on Linux or macOS:

    ```bash
    c++ -std=c++17 main.cc perfetto.cc -lpthread -o example
    ./example
    ```

4.  Open `example.pftrace` in [ui.perfetto.dev](https://ui.perfetto.dev). You
    should see three `DrawFrame` slices, each containing two `DrawPlayer`
    slices, and a `Framerate` counter.

For CMake and Windows builds, and for recording alongside a system trace, see
[Instrument a C++ app](/docs/getting-started/in-app-tracing.md).

## Next steps

### Instrument

- [Track events (C++ SDK)](/docs/instrumentation/track-events.md): slices, counters, flows and custom tracks in detail.
- [Redirect trace data with interceptors](/docs/instrumentation/interceptors.md): send trace packets to a custom backend, such as the console.

### Understand

- [Buffers and data flow](/docs/concepts/buffers.md): how events get from your code into the trace, and how to size buffers.
- [Clock synchronization](/docs/concepts/clock-sync.md): how timestamps from different clocks line up in one trace.

### Look up

- [Tracing SDK (C++)](/docs/instrumentation/tracing-sdk.md): the C++ SDK API, its backends and optional features.
