- [Overview](#)

  - [What is Perfetto?](README.md)
  - [What is tracing?](tracing-101.md)
  - [How do I start using Perfetto?](getting-started/start-using-perfetto.md)
  - [Community and getting help](community.md)

- [Getting started](#)

  - [Tutorials](#)

    - [System tracing](getting-started/system-tracing.md) {.tag-android .tag-linux}
    - [In-app tracing](getting-started/in-app-tracing.md) {.tag-cpp-rust}
    - [Rust SDK](getting-started/rust-sdk.md) {.tag-cpp-rust}
    - [Memory profiling](getting-started/memory-profiling.md) {.tag-android .tag-linux}
    - [CPU profiling](getting-started/cpu-profiling.md) {.tag-android .tag-linux}
    - [Instrumenting with atrace](getting-started/atrace.md) {.tag-android}
    - [Instrumenting with ftrace](getting-started/ftrace.md) {.tag-linux .tag-android}
    - [Recording Chrome traces](getting-started/chrome-tracing.md) {.tag-chrome}
    - [Importing other formats](getting-started/other-formats.md) {.tag-perf}
    - [Converting data to Perfetto](getting-started/converting.md) {.tag-perf}

  - [Cookbooks](#)

    - [Recording Android traces locally](getting-started/local-android-trace-recording.md) {.tag-android}
    - [Analyzing Android traces](getting-started/android-trace-analysis.md) {.tag-android}
    - [Analyzing traces from the command line](getting-started/command-line-analysis.md) {.tag-android .tag-linux .tag-cpp-rust .tag-chrome .tag-perf}
    - [Tracing on Linux](getting-started/linux-cookbook.md) {.tag-linux}
    - [Capturing periodic trace snapshots](getting-started/periodic-trace-snapshots.md) {.tag-android .tag-linux}
    - [Using AI with Perfetto](getting-started/using-ai.md) {.tag-android .tag-linux .tag-cpp-rust .tag-chrome .tag-perf}

  - [Case studies](#)

    - [Debugging memory usage](case-studies/memory.md) {.tag-android}
    - [Scheduling blockages](case-studies/scheduling-blockages.md) {.tag-android .tag-linux}

  - [Contributing](#)

    - [Getting started](contributing/getting-started.md) {.tag-contrib}
    - [Common tasks](contributing/common-tasks.md) {.tag-contrib}

- [Learning more](#)

  - [Concepts](#)

    - [Service model](concepts/service-model.md) {.tag-android .tag-linux .tag-cpp-rust}
    - [Buffers and dataflow](concepts/buffers.md) {.tag-android .tag-linux .tag-cpp-rust .tag-chrome}
    - [Trace configuration](concepts/config.md) {.tag-android .tag-linux .tag-cpp-rust .tag-chrome}
    - [Clock synchronization](concepts/clock-sync.md) {.tag-android .tag-linux .tag-cpp-rust .tag-chrome}
    - [Trace merging](concepts/merging-traces.md) {.tag-android .tag-linux .tag-cpp-rust .tag-chrome .tag-perf}
    - [Concurrent sessions](concepts/concurrent-tracing-sessions.md) {.tag-android .tag-linux .tag-cpp-rust}

  - [Recording](#)

    - [Tracing in background](learning-more/tracing-in-background.md) {.tag-android .tag-linux}
    - [Advanced Android tracing](learning-more/android.md) {.tag-android}
    - [Symbolization and deobfuscation](learning-more/symbolization.md) {.tag-android .tag-linux}
    - [Tracing across reboots](data-sources/previous-boot-trace.md) {.tag-android .tag-linux}
    - [Custom proto extensions](instrumentation/extensions.md) {.tag-cpp-rust .tag-android .tag-perf}
    - [heapprofd API](instrumentation/heapprofd-api.md) {.tag-cpp-rust}
    - [Multi-machine recording](learning-more/multi-machine-tracing.md) {.tag-android .tag-linux}

  - [Data sources](#)

    - [System](#)

      - [CPU scheduling](data-sources/cpu-scheduling.md) {.tag-android .tag-linux}
      - [System calls](data-sources/syscalls.md) {.tag-android .tag-linux}
      - [CPU frequency](data-sources/cpu-freq.md) {.tag-android .tag-linux}
      - [Function graph](data-sources/funcgraph.md) {.tag-android .tag-linux}
      - [GPU](data-sources/gpu.md) {.tag-android .tag-linux .tag-perf}

    - [Memory](#)

      - [Memory counters](data-sources/memory-counters.md) {.tag-android .tag-linux}
      - [Allocation profiler](data-sources/native-heap-profiler.md) {.tag-android .tag-linux}
      - [ART heap dumps](data-sources/java-heap-profiler.md) {.tag-android}

    - [Android](#)

      - [ATrace](data-sources/atrace.md) {.tag-android}
      - [Logcat](data-sources/android-log.md) {.tag-android}
      - [Frame timeline](data-sources/frametimeline.md) {.tag-android}
      - [Battery & power](data-sources/battery-counters.md) {.tag-android}
      - [Android game interventions](data-sources/android-game-intervention-list.md) {.tag-android}
      - [Android aflags](data-sources/android-aflags.md) {.tag-android}
      - [Screen recording](data-sources/video-frames.md) {.tag-android}

  - [Tracing SDK](#)

    - [Tracing SDK](instrumentation/tracing-sdk.md) {.tag-cpp-rust}
    - [Track events](instrumentation/track-events.md) {.tag-cpp-rust}

  - [Visualization](#)

    - [Perfetto UI](visualization/perfetto-ui.md) {.tag-android .tag-linux .tag-cpp-rust .tag-chrome .tag-perf}
    - [Merging traces in the UI](visualization/merging-traces.md) {.tag-android .tag-linux .tag-cpp-rust .tag-chrome .tag-perf}
    - [Data Explorer](visualization/data-explorer.md) {.tag-android .tag-linux .tag-cpp-rust .tag-chrome .tag-perf}
    - [Opening large traces](visualization/large-traces.md) {.tag-android .tag-linux .tag-cpp-rust .tag-chrome .tag-perf}
    - [Deep linking](visualization/deep-linking-to-perfetto-ui.md) {.tag-android .tag-linux .tag-cpp-rust .tag-chrome .tag-perf}
    - [Embedding the UI](visualization/embedding-the-ui.md) {.tag-android .tag-linux .tag-cpp-rust .tag-chrome .tag-perf}
    - [Debug tracks](analysis/debug-tracks.md) {.tag-android .tag-linux .tag-cpp-rust .tag-chrome .tag-perf}
    - [Heap Dump Explorer](visualization/heap-dump-explorer.md) {.tag-android}
    - [Memscope & memory overview](visualization/memscope.md) {.tag-android .tag-linux}

    - [Extending the UI](#)

      - [Overview](visualization/extending-the-ui.md) {.tag-android .tag-linux .tag-cpp-rust .tag-perf}
      - [UI automation](visualization/ui-automation.md) {.tag-android .tag-linux .tag-cpp-rust .tag-perf}
      - [Commands reference](visualization/commands-automation-reference.md) {.tag-android .tag-linux .tag-cpp-rust .tag-perf}
      - [Extension servers](visualization/extension-servers.md) {.tag-android .tag-linux .tag-cpp-rust .tag-perf}
      - [Googler features](visualization/googler-features.md) {.tag-android .tag-linux .tag-cpp-rust .tag-perf}

  - [Trace analysis](#)

    - [Getting started](analysis/getting-started.md) {.tag-android .tag-linux .tag-cpp-rust .tag-chrome .tag-perf}

    - [PerfettoSQL](#)

      - [Getting started](analysis/perfetto-sql-getting-started.md) {.tag-android .tag-linux .tag-cpp-rust .tag-chrome .tag-perf}
      - [Syntax](analysis/perfetto-sql-syntax.md) {.tag-android .tag-linux .tag-cpp-rust .tag-chrome .tag-perf}
      - [Standard library](analysis/stdlib-docs.autogen) {.tag-android .tag-linux .tag-cpp-rust .tag-chrome .tag-perf}
      - [Style guide](analysis/style-guide.md) {.tag-android .tag-linux .tag-cpp-rust .tag-chrome .tag-perf}
      - [Backwards compatibility](analysis/perfetto-sql-backcompat.md) {.tag-android .tag-linux .tag-cpp-rust .tag-chrome .tag-perf}

    - [Trace Processor](#)

      - [C++ library](analysis/trace-processor.md) {.tag-android .tag-linux .tag-cpp-rust .tag-chrome .tag-perf}
      - [Python library](analysis/trace-processor-python.md) {.tag-android .tag-linux .tag-cpp-rust .tag-chrome .tag-perf}
      - [Batch Trace Processor](analysis/batch-trace-processor.md) {.tag-android .tag-linux .tag-cpp-rust .tag-chrome .tag-perf}

    - [Merging traces from the command line](analysis/merging-traces.md) {.tag-android .tag-linux .tag-cpp-rust .tag-chrome .tag-perf}
    - [Trace summarization](analysis/trace-summary.md) {.tag-android .tag-linux .tag-cpp-rust .tag-chrome .tag-perf}
    - [Converting from Perfetto](quickstart/traceconv.md) {.tag-android .tag-linux .tag-cpp-rust .tag-chrome}

  - [FAQ](faq.md) {.tag-android .tag-linux .tag-cpp-rust .tag-chrome .tag-perf}

- [Diving deep](#)

  - [CLI tools](#)

    - [trace_processor](reference/trace-processor-cli.md)
    - [perfetto](reference/perfetto-cli.md) {.tag-android .tag-linux}
    - [traced](reference/traced.md) {.tag-android .tag-linux}
    - [traced_probes](reference/traced_probes.md) {.tag-android .tag-linux}
    - [heap_profile](reference/heap_profile-cli.md) {.tag-android .tag-linux}
    - [tracebox](reference/tracebox.md) {.tag-android .tag-linux}

  - [Reference](#)

    - [Protos](#)

      - [Trace config](reference/trace-config-proto.autogen) {.tag-android .tag-linux .tag-cpp-rust .tag-chrome}
      - [Trace packet](reference/trace-packet-proto.autogen) {.tag-android .tag-linux .tag-cpp-rust .tag-chrome .tag-perf}

    - [PerfettoSQL](#)

      - [Prelude tables](analysis/sql-tables.autogen) {.tag-android .tag-linux .tag-cpp-rust .tag-chrome .tag-perf}
      - [Built-in functions](analysis/builtin.md) {.tag-android .tag-linux .tag-cpp-rust .tag-chrome .tag-perf}
      - [Stats table](analysis/sql-stats.autogen) {.tag-android .tag-linux .tag-cpp-rust .tag-chrome .tag-perf}

    - [Synthetic track events](reference/synthetic-track-event.md) {.tag-perf}
    - [Kernel track events](reference/kernel-track-event.md) {.tag-android .tag-linux}
    - [Trace manifest format](reference/perfetto-manifest.md) {.tag-android .tag-linux .tag-cpp-rust .tag-chrome .tag-perf}
    - [Extension server protocol](visualization/extension-server-protocol.md) {.tag-android .tag-linux .tag-cpp-rust .tag-chrome .tag-perf}
    - [Embedding API](visualization/embedding-api-reference.md) {.tag-android .tag-linux .tag-cpp-rust .tag-chrome .tag-perf}
    - [Android version notes](reference/android-version-notes.md) {.tag-android}

  - [Advanced topics](#)

    - [Detached mode](concepts/detached-mode.md) {.tag-android}
    - [Interceptors](instrumentation/interceptors.md) {.tag-cpp-rust}
    - [Legacy (v1) metrics](analysis/metrics.md) {.tag-android}
    - [Multi-machine architecture](deployment/multi-machine-architecture.md) {.tag-android .tag-linux}
    - [BigTrace (single machine)](deployment/deploying-bigtrace-on-a-single-machine.md) {.tag-android .tag-perf}
    - [BigTrace on Kubernetes](deployment/deploying-bigtrace-on-kubernetes.md) {.tag-android .tag-perf}

  - [Contributing](#)

    - [Building](contributing/build-instructions.md) {.tag-contrib}
    - [Testing](contributing/testing.md) {.tag-contrib}

    - [UI development](#)

      - [Getting started](contributing/ui-getting-started.md) {.tag-contrib}
      - [Plugins](contributing/ui-plugins.md) {.tag-contrib}

    - [Releases](#)

      - [SDK release](contributing/sdk-releasing.md) {.tag-contrib}
      - [Python release](contributing/python-releasing.md) {.tag-contrib}
      - [UI release](visualization/perfetto-ui-release-process.md) {.tag-contrib}

    - [Become a committer](contributing/become-a-committer.md) {.tag-contrib}
    - [Chrome branches](contributing/chrome-branches.md) {.tag-contrib}
    - [SQLite upgrade](contributing/sqlite-upgrade-guide.md) {.tag-contrib}

  - [Design documents](#)

    - [Core](#)

      - [API and ABI surface](design-docs/api-and-abi.md) {.tag-contrib}
      - [Life of a tracing session](design-docs/life-of-a-tracing-session.md) {.tag-contrib}
      - [Security model](design-docs/security-model.md) {.tag-contrib}
      - [Trace buffer V2](design-docs/trace-buffer.md) {.tag-contrib}

    - [Infrastructure](#)

      - [ProtoZero](design-docs/protozero.md) {.tag-contrib}
      - [LockFreeTaskRunner](design-docs/lock-free-task-runner.md) {.tag-contrib}

    - [Trace Processor](#)

      - [Architecture](design-docs/trace-processor-architecture.md) {.tag-contrib}
      - [Batch Trace Processor](design-docs/batch-trace-processor.md) {.tag-contrib}

    - [UI](#)

      - [Data Explorer architecture](design-docs/data-explorer-architecture.md) {.tag-contrib}

    - [Profiling](#)

      - [Heapprofd design](design-docs/heapprofd-design.md) {.tag-contrib}
      - [Heapprofd wire protocol](design-docs/heapprofd-wire-protocol.md) {.tag-contrib}
      - [Heapprofd sampling](design-docs/heapprofd-sampling.md) {.tag-contrib}
      - [pprof support](design-docs/pprof-support.md) {.tag-contrib}

    - [Other](#)

      - [Statsd checkpoint atoms](design-docs/checkpoint-atoms.md) {.tag-contrib}
      - [Perfetto CI](design-docs/continuous-integration.md) {.tag-contrib}
