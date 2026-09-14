- [Start here](#)

  - [What is Perfetto?](README.md) {.type-concept}
  - [Tracing and profiling, explained](tracing-101.md) {.type-concept}
  - [Perfetto for Android developers](start/android.md) {.tag-android .type-start}
  - [Perfetto for Linux developers](start/linux.md) {.tag-linux .type-start}
  - [Perfetto for C, C++ and Rust developers](start/cpp-rust.md) {.tag-cpp-rust .type-start}
  - [Perfetto for Chromium developers](start/chrome.md) {.tag-chrome .type-start}
  - [Perfetto for performance engineers](start/performance.md) {.tag-performance .type-start}

- [Tutorials](#)

  - [Record](#)

    - [Record your first system trace](getting-started/system-tracing.md) {.tag-android .tag-linux .type-tutorial}

  - [Instrument](#)

    - [Instrument a C++ app](getting-started/in-app-tracing.md) {.tag-cpp-rust .type-tutorial}
    - [Instrument a C app](tutorials/c-sdk.md) {.tag-cpp-rust .type-tutorial .planned}
    - [Instrument a Rust app](getting-started/rust-sdk.md) {.tag-cpp-rust .type-tutorial}
    - [Instrument the Linux kernel with ftrace](getting-started/ftrace.md) {.tag-linux .type-tutorial}

  - [Profile](#)

    - [Record CPU profiles and perf counters](getting-started/cpu-profiling.md) {.tag-android .tag-linux .tag-performance .type-tutorial}
    - [Profile native memory on Android](getting-started/memory-profiling.md) {.tag-android .type-tutorial}

  - [Open & convert](#)

    - [Convert your own data to a trace](getting-started/converting.md) {.tag-performance .tag-cpp-rust .type-tutorial}

  - [Explore](#)

    - [Tour the Perfetto UI](visualization/perfetto-ui.md) {.type-tutorial}

  - [Analyze](#)

    - [Query traces with PerfettoSQL](analysis/perfetto-sql-getting-started.md) {.type-tutorial}

- [How-to guides](#)

  - [Record](#)

    - [Choose how to record](how-to/choose-how-to-record.md) {.tag-android .tag-linux .type-howto}
    - [Record from the command line on Android](learning-more/android.md) {.tag-android .type-howto}
    - [Record in the background](learning-more/tracing-in-background.md) {.tag-android .tag-linux .type-howto}
    - [Take periodic trace snapshots](getting-started/periodic-trace-snapshots.md) {.tag-android .tag-linux .type-howto}
    - [Record boot traces and heap dumps on OOM](getting-started/local-android-trace-recording.md) {.tag-android .type-howto}
    - [Capture ftrace data across a reboot](data-sources/previous-boot-trace.md) {.tag-linux .type-howto}
    - [Trace multiple machines](learning-more/multi-machine-tracing.md) {.tag-linux .type-howto}
    - [Record a Chrome trace](getting-started/chrome-tracing.md) {.tag-chrome .type-howto}

  - [Instrument](#)

    - [Choose an SDK: C, C++ or Rust](how-to/choose-an-sdk.md) {.tag-cpp-rust .type-howto .planned}
    - [Instrument Android code with atrace](getting-started/atrace.md) {.tag-android .type-howto}
    - [Add custom fields with proto extensions](instrumentation/extensions.md) {.tag-cpp-rust .type-howto}
    - [Redirect trace data with interceptors](instrumentation/interceptors.md) {.tag-cpp-rust .type-howto}
    - [Profile a custom allocator](instrumentation/heapprofd-api.md) {.tag-cpp-rust .type-howto}

  - [Profile](#)

    - [Choose a profiler](how-to/choose-a-profiler.md) {.tag-android .tag-linux .tag-performance .type-howto}
    - [Take an ART heap dump](how-to/art-heap-dump.md) {.tag-android .type-howto}
    - [Profile and trace native code on Linux](getting-started/linux-cookbook.md) {.tag-linux .type-howto}
    - [Monitor memory live with Memscope](visualization/memscope.md) {.tag-android .type-howto}
    - [Symbolize and deobfuscate profiles](learning-more/symbolization.md) {.tag-android .tag-linux .tag-performance .type-howto}

  - [Open & convert](#)

    - [Open a pprof, perf, simpleperf or samply profile](getting-started/viewing-cpu-profiles.md) {.tag-performance .tag-linux .type-howto}
    - [Merge traces in the UI](visualization/merging-traces.md) {.tag-performance .tag-android .tag-linux .type-howto}
    - [Merge traces from the command line](analysis/merging-traces.md) {.tag-performance .type-howto}
    - [Generate traces programmatically: advanced recipes](reference/synthetic-track-event.md) {.tag-performance .type-howto}

  - [Explore](#)

    - [Open large traces](visualization/large-traces.md) {.type-howto}
    - [Turn query results into debug tracks](analysis/debug-tracks.md) {.type-howto}
    - [Explore data with the Data Explorer](visualization/data-explorer.md) {.tag-performance .type-howto}

  - [Analyze](#)

    - [Analyze Android traces with SQL](getting-started/android-trace-analysis.md) {.tag-android .type-howto}
    - [Work with traces from the command line](getting-started/command-line-analysis.md) {.tag-performance .tag-linux .type-howto}
    - [Use the Trace Processor shell and C++ library](analysis/trace-processor.md) {.tag-cpp-rust .tag-performance .type-howto}
    - [Analyze traces from Python](analysis/trace-processor-python.md) {.tag-performance .type-howto}
    - [Analyze many traces at once](analysis/batch-trace-processor.md) {.tag-performance .type-howto}
    - [Summarize traces into metrics](analysis/trace-summary.md) {.tag-performance .type-howto}
    - [Use Perfetto with AI agents](getting-started/using-ai.md) {.type-howto}
    - [Deploy BigTrace on a single machine](deployment/deploying-bigtrace-on-a-single-machine.md) {.tag-performance .type-howto}
    - [Deploy BigTrace on Kubernetes](deployment/deploying-bigtrace-on-kubernetes.md) {.tag-performance .type-howto}

  - [Integrate](#)

    - [Choose how to extend the UI](visualization/extending-the-ui.md) {.tag-performance .type-howto}
    - [Automate the UI with commands and macros](visualization/ui-automation.md) {.tag-performance .type-howto}
    - [Share macros and SQL modules with extension servers](visualization/extension-servers.md) {.tag-performance .type-howto}
    - [Open traces in Perfetto from your tool](visualization/deep-linking-to-perfetto-ui.md) {.tag-performance .type-howto}
    - [Embed the Perfetto UI](visualization/embedding-the-ui.md) {.tag-performance .type-howto}

  - [Solve Android problems](#)

    - [Investigate jank](how-to/investigate-jank.md) {.tag-android .type-howto .planned}
    - [Investigate slow app startup](how-to/investigate-startup.md) {.tag-android .type-howto .planned}
    - [Investigate memory use on Android](case-studies/memory.md) {.tag-android .type-howto}
    - [Investigate a blocked thread](how-to/investigate-blocked-thread.md) {.tag-android .tag-linux .type-howto .planned}
    - [Case study: a SystemUI scheduling blockage](case-studies/scheduling-blockages.md) {.tag-android .tag-linux .type-howto}

- [Concepts](#)

  - [Record](#)

    - [How Perfetto works](concepts/service-model.md) {.tag-android .tag-linux .tag-cpp-rust .type-concept}
    - [Trace configuration](concepts/config.md) {.tag-android .tag-linux .tag-cpp-rust .type-concept}
    - [Buffers and data flow](concepts/buffers.md) {.tag-android .tag-linux .tag-cpp-rust .type-concept}
    - [Concurrent tracing sessions](concepts/concurrent-tracing-sessions.md) {.tag-android .tag-linux .type-concept}
    - [Multi-machine architecture](deployment/multi-machine-architecture.md) {.tag-linux .type-concept}

  - [Open, explore & analyze](#)

    - [Clock synchronization](concepts/clock-sync.md) {.tag-cpp-rust .tag-performance .type-concept}
    - [How traces are merged](concepts/merging-traces.md) {.tag-performance .tag-linux .type-concept}
    - [How trace analysis works](analysis/getting-started.md) {.type-concept}

- [Reference](#)

  - [Data sources](#)

    - [Data sources index](reference/data-sources.md) {.tag-android .tag-linux .type-reference}

    - [System](#)

      - [CPU scheduling](data-sources/cpu-scheduling.md) {.tag-android .tag-linux .type-reference}
      - [System calls](data-sources/syscalls.md) {.tag-android .tag-linux .type-reference}
      - [CPU frequency and idle states](data-sources/cpu-freq.md) {.tag-android .tag-linux .type-reference}
      - [Kernel function graph](data-sources/funcgraph.md) {.tag-linux .type-reference}
      - [CPU profiler (linux.perf)](data-sources/linux-perf.md) {.tag-android .tag-linux .tag-performance .type-reference .planned}
      - [GPU](data-sources/gpu.md) {.tag-android .tag-linux .type-reference}

    - [Memory](#)

      - [Memory counters and events](data-sources/memory-counters.md) {.tag-android .tag-linux .type-reference}
      - [Native heap profiler (heapprofd)](data-sources/native-heap-profiler.md) {.tag-android .tag-linux .type-reference}
      - [ART heap dumps](data-sources/java-heap-profiler.md) {.tag-android .type-reference}

    - [Android](#)

      - [ATrace](data-sources/atrace.md) {.tag-android .type-reference}
      - [Logcat](data-sources/android-log.md) {.tag-android .type-reference}
      - [FrameTimeline](data-sources/frametimeline.md) {.tag-android .type-reference}
      - [Battery and power rails](data-sources/battery-counters.md) {.tag-android .type-reference}
      - [Screen recording](data-sources/video-frames.md) {.tag-android .type-reference}
      - [Aflags](data-sources/android-aflags.md) {.tag-android .type-reference}
      - [Game interventions](data-sources/android-game-intervention-list.md) {.tag-android .type-reference}
      - [Packages, properties and other device state](data-sources/device-state.md) {.tag-android .type-reference .planned}

    - [Apps](#)

      - [Track events (track_event)](data-sources/track-event.md) {.tag-cpp-rust .tag-android .type-reference .planned}

  - [Trace config and protos](#)

    - [TraceConfig](reference/trace-config-proto.autogen) {.tag-android .tag-linux .tag-cpp-rust .tag-chrome .type-reference}
    - [TracePacket](reference/trace-packet-proto.autogen) {.tag-cpp-rust .tag-performance .type-reference}
    - [Android version notes](reference/android-version-notes.md) {.tag-android .type-reference}

  - [PerfettoSQL](#)

    - [PerfettoSQL syntax](analysis/perfetto-sql-syntax.md) {.type-reference}
    - [Prelude tables](analysis/sql-tables.autogen) {.type-reference}
    - [Standard library](analysis/stdlib-docs.autogen) {.type-reference}
    - [Built-in functions](analysis/builtin.md) {.type-reference}
    - [Stats table](analysis/sql-stats.autogen) {.type-reference}
    - [Backwards compatibility](analysis/perfetto-sql-backcompat.md) {.tag-performance .tag-android .type-reference}
    - [Legacy (v1) metrics](analysis/metrics.md) {.tag-android .type-reference}

  - [Perfetto UI](#)

    - [Pages and panels](reference/ui-pages.md) {.type-reference .planned}
    - [Flamegraph and tree explorer](reference/flamegraph.md) {.type-reference .planned}
    - [Heap Dump Explorer](visualization/heap-dump-explorer.md) {.tag-android .type-reference}
    - [UI commands](visualization/commands-automation-reference.md) {.tag-performance .type-reference}
    - [Embedding API](visualization/embedding-api-reference.md) {.tag-performance .type-reference}
    - [Extension server protocol](visualization/extension-server-protocol.md) {.tag-performance .type-reference}

  - [SDKs and trace formats](#)

    - [Track events (C++ SDK)](instrumentation/track-events.md) {.tag-cpp-rust .type-reference}
    - [Tracing SDK (C++)](instrumentation/tracing-sdk.md) {.tag-cpp-rust .type-reference}
    - [C SDK API](reference/c-sdk-api.md) {.tag-cpp-rust .type-reference .planned}
    - [Supported trace and profile formats](getting-started/other-formats.md) {.tag-performance .tag-android .tag-linux .type-reference}
    - [Kernel track events](reference/kernel-track-event.md) {.tag-linux .type-reference}
    - [Trace manifest format](reference/perfetto-manifest.md) {.tag-performance .type-reference}

  - [Command-line tools](#)

    - [perfetto](reference/perfetto-cli.md) {.tag-android .tag-linux .type-reference}
    - [tracebox](reference/tracebox.md) {.tag-linux .type-reference}
    - [traced](reference/traced.md) {.tag-android .tag-linux .type-reference}
    - [traced_probes](reference/traced_probes.md) {.tag-android .tag-linux .type-reference}
    - [heap_profile](reference/heap_profile-cli.md) {.tag-android .tag-linux .type-reference}
    - [record_android_trace](reference/record-android-trace.md) {.tag-android .type-reference .planned}
    - [trace_processor](reference/trace-processor-cli.md) {.type-reference}
    - [Environment variables](reference/environment-variables.md) {.tag-android .tag-linux .tag-cpp-rust .type-reference .planned}

  - [More](#)

    - [Glossary](reference/glossary.md) {.type-reference .planned}

- [Contributing](#)

  - [Start contributing](contributing/getting-started.md) {.tag-contrib .type-start}
  - [Build from source](contributing/build-instructions.md) {.tag-contrib .type-howto}
  - [Testing](contributing/testing.md) {.tag-contrib .type-howto}
  - [Common tasks](contributing/common-tasks.md) {.tag-contrib .type-howto}

  - [UI development](#)

    - [Set up UI development](contributing/ui-getting-started.md) {.tag-contrib .type-howto}
    - [UI plugins](contributing/ui-plugins.md) {.tag-contrib .type-howto}

  - [Releases](#)

    - [Release the SDK](contributing/sdk-releasing.md) {.tag-contrib .type-howto}
    - [Release the Python library](contributing/python-releasing.md) {.tag-contrib .type-howto}
    - [Release the UI](visualization/perfetto-ui-release-process.md) {.tag-contrib .type-howto}
    - [Branch for a Chrome milestone](contributing/chrome-branches.md) {.tag-contrib .type-howto}
    - [Upgrade SQLite](contributing/sqlite-upgrade-guide.md) {.tag-contrib .type-howto}

  - [Become a committer](contributing/become-a-committer.md) {.tag-contrib .type-concept}

  - [Design documents](#)

    - [Core](#)

      - [API and ABI surface](design-docs/api-and-abi.md) {.tag-contrib .type-concept}
      - [Life of a tracing session](design-docs/life-of-a-tracing-session.md) {.tag-contrib .type-concept}
      - [Security model](design-docs/security-model.md) {.tag-contrib .type-concept}
      - [Trace Buffer V2](design-docs/trace-buffer.md) {.tag-contrib .type-concept}

    - [Infrastructure](#)

      - [ProtoZero](design-docs/protozero.md) {.tag-contrib .type-concept}
      - [LockFreeTaskRunner](design-docs/lock-free-task-runner.md) {.tag-contrib .type-concept}

    - [Trace Processor](#)

      - [Trace Processor architecture](design-docs/trace-processor-architecture.md) {.tag-contrib .type-concept}
      - [Batch Trace Processor design](design-docs/batch-trace-processor.md) {.tag-contrib .type-concept}

    - [UI](#)

      - [Data Explorer architecture](design-docs/data-explorer-architecture.md) {.tag-contrib .type-concept}

    - [Profiling](#)

      - [Heapprofd design](design-docs/heapprofd-design.md) {.tag-contrib .type-concept}
      - [Heapprofd wire protocol](design-docs/heapprofd-wire-protocol.md) {.tag-contrib .type-concept}
      - [Heapprofd sampling](design-docs/heapprofd-sampling.md) {.tag-contrib .type-concept}
      - [pprof support](design-docs/pprof-support.md) {.tag-contrib .type-concept}

    - [Other](#)

      - [Statsd checkpoint atoms](design-docs/checkpoint-atoms.md) {.tag-contrib .type-concept}
      - [Perfetto CI](design-docs/continuous-integration.md) {.tag-contrib .type-concept}
