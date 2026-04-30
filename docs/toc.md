- [Overview](#)

  - [What is Perfetto?](README.md)
  - [What is Tracing?](tracing-101.md)
  - [How do I start using Perfetto?](getting-started/start-using-perfetto.md)

- [Getting Started](#)

  - [Tutorials](#)

    - [System Tracing](getting-started/system-tracing.md) {.tag-android .tag-linux}
    - [In-App Tracing](getting-started/in-app-tracing.md) {.tag-cpp}
    - [Memory Profiling](getting-started/memory-profiling.md) {.tag-android .tag-linux}
    - [CPU Profiling](getting-started/cpu-profiling.md) {.tag-android .tag-linux}
    - [Instrumenting with atrace](getting-started/atrace.md) {.tag-android}
    - [Instrumenting with ftrace](getting-started/ftrace.md) {.tag-linux .tag-android}
    - [Recording Chrome Traces](getting-started/chrome-tracing.md) {.tag-chrome}
    - [Importing Other Formats](getting-started/other-formats.md) {.tag-perf}
    - [Converting Data to Perfetto](getting-started/converting.md) {.tag-perf}

  - [Cookbooks](#)

    - [Analysing Android Traces](getting-started/android-trace-analysis.md) {.tag-android}
    - [Periodic Trace Snapshots](getting-started/periodic-trace-snapshots.md) {.tag-android .tag-linux}
    - [Boot Tracing](case-studies/android-boot-tracing.md) {.tag-android}
    - [OutOfMemoryError](case-studies/android-outofmemoryerror.md) {.tag-android}

  - [Case Studies](#)

    - [Debugging Memory Usage](case-studies/memory.md) {.tag-android}
    - [Scheduling Blockages](case-studies/scheduling-blockages.md) {.tag-android .tag-linux}

  - [Contributing](#)

    - [Getting Started](contributing/getting-started.md) {.tag-contrib}
    - [Common Tasks](contributing/common-tasks.md) {.tag-contrib}

- [Learning More](#)

  - [Concepts](#)

    - [Service Model](concepts/service-model.md) {.tag-android .tag-linux .tag-cpp}
    - [Buffers and Dataflow](concepts/buffers.md) {.tag-android .tag-linux .tag-cpp .tag-chrome}
    - [Trace Configuration](concepts/config.md) {.tag-android .tag-linux .tag-cpp .tag-chrome}
    - [Clock Synchronization](concepts/clock-sync.md) {.tag-android .tag-linux .tag-cpp .tag-chrome}
    - [Concurrent Sessions](concepts/concurrent-tracing-sessions.md) {.tag-android .tag-linux .tag-cpp}

  - [Recording](#)

    - [Tracing in Background](learning-more/tracing-in-background.md) {.tag-android .tag-linux}
    - [Advanced Android Tracing](learning-more/android.md) {.tag-android}
    - [Symbolization and Deobfuscation](learning-more/symbolization.md) {.tag-android .tag-linux}
    - [Tracing across Reboots](data-sources/previous-boot-trace.md) {.tag-android .tag-linux}
    - [Custom Proto Extensions](instrumentation/extensions.md) {.tag-cpp .tag-android .tag-perf}
    - [heapprofd API](instrumentation/heapprofd-api.md) {.tag-cpp}

  - [Data Sources](#)

    - [System](#)

      - [CPU Scheduling](data-sources/cpu-scheduling.md) {.tag-android .tag-linux}
      - [System Calls](data-sources/syscalls.md) {.tag-android .tag-linux}
      - [CPU Frequency](data-sources/cpu-freq.md) {.tag-android .tag-linux}
      - [GPU](data-sources/gpu.md) {.tag-android .tag-linux .tag-perf}

    - [Memory](#)

      - [Memory Counters](data-sources/memory-counters.md) {.tag-android .tag-linux}
      - [Allocation Profiler](data-sources/native-heap-profiler.md) {.tag-android .tag-linux}
      - [ART Heap Dumps](data-sources/java-heap-profiler.md) {.tag-android}

    - [Android](#)

      - [ATrace](data-sources/atrace.md) {.tag-android}
      - [Logcat](data-sources/android-log.md) {.tag-android}
      - [Frame Timeline](data-sources/frametimeline.md) {.tag-android}
      - [Battery & Power](data-sources/battery-counters.md) {.tag-android}
      - [Android Game Interventions](data-sources/android-game-intervention-list.md) {.tag-android}
      - [Android Aflags](data-sources/android-aflags.md) {.tag-android}

  - [Tracing SDK](#)

    - [Tracing SDK](instrumentation/tracing-sdk.md) {.tag-cpp}
    - [Track Events](instrumentation/track-events.md) {.tag-cpp}

  - [Visualization](#)

    - [Perfetto UI](visualization/perfetto-ui.md) {.tag-android .tag-linux .tag-cpp .tag-chrome .tag-perf}
    - [Opening Large Traces](visualization/large-traces.md) {.tag-android .tag-linux .tag-cpp .tag-chrome .tag-perf}
    - [Deep Linking](visualization/deep-linking-to-perfetto-ui.md) {.tag-android .tag-linux .tag-cpp .tag-chrome .tag-perf}
    - [Debug Tracks](analysis/debug-tracks.md) {.tag-android .tag-linux .tag-cpp .tag-chrome .tag-perf}
    - [Heap Dump Explorer](visualization/heap-dump-explorer.md) {.tag-android}

    - [Extending the UI](#)

      - [Overview](visualization/extending-the-ui.md) {.tag-android .tag-linux .tag-cpp .tag-perf}
      - [UI Automation](visualization/ui-automation.md) {.tag-android .tag-linux .tag-cpp .tag-perf}
      - [Commands Reference](visualization/commands-automation-reference.md) {.tag-android .tag-linux .tag-cpp .tag-perf}
      - [Extension Servers](visualization/extension-servers.md) {.tag-android .tag-linux .tag-cpp .tag-perf}

  - [Trace Analysis](#)

    - [Getting Started](analysis/getting-started.md) {.tag-android .tag-linux .tag-cpp .tag-chrome .tag-perf}

    - [PerfettoSQL](#)

      - [Getting Started](analysis/perfetto-sql-getting-started.md) {.tag-android .tag-linux .tag-cpp .tag-chrome .tag-perf}
      - [Syntax](analysis/perfetto-sql-syntax.md) {.tag-android .tag-linux .tag-cpp .tag-chrome .tag-perf}
      - [Standard Library](analysis/stdlib-docs.autogen) {.tag-android .tag-linux .tag-cpp .tag-chrome .tag-perf}
      - [Style Guide](analysis/style-guide.md) {.tag-android .tag-linux .tag-cpp .tag-chrome .tag-perf}
      - [Backwards Compatibility](analysis/perfetto-sql-backcompat.md) {.tag-android .tag-linux .tag-cpp .tag-chrome .tag-perf}

    - [Trace Processor](#)

      - [C++ Library](analysis/trace-processor.md) {.tag-android .tag-linux .tag-cpp .tag-chrome .tag-perf}
      - [Python Library](analysis/trace-processor-python.md) {.tag-android .tag-linux .tag-cpp .tag-chrome .tag-perf}
      - [Batch Trace Processor](analysis/batch-trace-processor.md) {.tag-android .tag-linux .tag-cpp .tag-chrome .tag-perf}

    - [Trace Summarization](analysis/trace-summary.md) {.tag-android .tag-linux .tag-cpp .tag-chrome .tag-perf}
    - [Converting from Perfetto](quickstart/traceconv.md) {.tag-android .tag-linux .tag-cpp .tag-chrome}

  - [FAQ](faq.md) {.tag-android .tag-linux .tag-cpp .tag-chrome .tag-perf}

- [Diving Deep](#)

  - [CLI Tools](#)

    - [perfetto](reference/perfetto-cli.md) {.tag-android .tag-linux}
    - [traced](reference/traced.md) {.tag-android .tag-linux}
    - [traced_probes](reference/traced_probes.md) {.tag-android .tag-linux}
    - [heap_profile](reference/heap_profile-cli.md) {.tag-android .tag-linux}
    - [tracebox](reference/tracebox.md) {.tag-android .tag-linux}

  - [Reference](#)

    - [Protos](#)

      - [Trace Config](reference/trace-config-proto.autogen) {.tag-android .tag-linux .tag-cpp .tag-chrome}
      - [Trace Packet](reference/trace-packet-proto.autogen) {.tag-android .tag-linux .tag-cpp .tag-chrome .tag-perf}

    - [PerfettoSQL](#)

      - [Prelude Tables](analysis/sql-tables.autogen) {.tag-android .tag-linux .tag-cpp .tag-chrome .tag-perf}
      - [Built-in Functions](analysis/builtin.md) {.tag-android .tag-linux .tag-cpp .tag-chrome .tag-perf}
      - [Stats Table](analysis/sql-stats.autogen) {.tag-android .tag-linux .tag-cpp .tag-chrome .tag-perf}

    - [Synthetic Track Events](reference/synthetic-track-event.md) {.tag-perf}
    - [Kernel Track Events](reference/kernel-track-event.md) {.tag-android .tag-linux}
    - [Extension Server Protocol](visualization/extension-server-protocol.md) {.tag-android .tag-linux .tag-cpp .tag-chrome .tag-perf}
    - [Android Version Notes](reference/android-version-notes.md) {.tag-android}

  - [Advanced Topics](#)

    - [Detached Mode](concepts/detached-mode.md) {.tag-android}
    - [Interceptors](instrumentation/interceptors.md) {.tag-cpp}
    - [Legacy (v1) Metrics](analysis/metrics.md) {.tag-android}
    - [BigTrace (Single Machine)](deployment/deploying-bigtrace-on-a-single-machine.md) {.tag-android .tag-perf}
    - [BigTrace on Kubernetes](deployment/deploying-bigtrace-on-kubernetes.md) {.tag-android .tag-perf}

  - [Contributing](#)

    - [Building](contributing/build-instructions.md) {.tag-contrib}
    - [Testing](contributing/testing.md) {.tag-contrib}
    - [Developer Tools](contributing/developer-tools.md) {.tag-contrib}

    - [UI Development](#)

      - [Getting Started](contributing/ui-getting-started.md) {.tag-contrib}
      - [Plugins](contributing/ui-plugins.md) {.tag-contrib}

    - [Releases](#)

      - [SDK Release](contributing/sdk-releasing.md) {.tag-contrib}
      - [Python Release](contributing/python-releasing.md) {.tag-contrib}
      - [UI Release](visualization/perfetto-ui-release-process.md) {.tag-contrib}

    - [Become a Committer](contributing/become-a-committer.md) {.tag-contrib}
    - [Chrome Branches](contributing/chrome-branches.md) {.tag-contrib}
    - [SQLite Upgrade](contributing/sqlite-upgrade-guide.md) {.tag-contrib}

  - [Design Documents](#)

    - [Core](#)

      - [API and ABI Surface](design-docs/api-and-abi.md) {.tag-contrib}
      - [Life of a Tracing Session](design-docs/life-of-a-tracing-session.md) {.tag-contrib}
      - [Security Model](design-docs/security-model.md) {.tag-contrib}
      - [Trace Buffer V2](design-docs/trace-buffer.md) {.tag-contrib}

    - [Infrastructure](#)

      - [ProtoZero](design-docs/protozero.md) {.tag-contrib}
      - [LockFreeTaskRunner](design-docs/lock-free-task-runner.md) {.tag-contrib}

    - [Trace Processor](#)

      - [Architecture](design-docs/trace-processor-architecture.md) {.tag-contrib}
      - [Batch Trace Processor](design-docs/batch-trace-processor.md) {.tag-contrib}

    - [UI](#)

      - [Data Explorer Architecture](design-docs/data-explorer-architecture.md) {.tag-contrib}

    - [Profiling](#)

      - [Heapprofd Design](design-docs/heapprofd-design.md) {.tag-contrib}
      - [Heapprofd Wire Protocol](design-docs/heapprofd-wire-protocol.md) {.tag-contrib}
      - [Heapprofd Sampling](design-docs/heapprofd-sampling.md) {.tag-contrib}
      - [pprof Support](design-docs/pprof-support.md) {.tag-contrib}

    - [Other](#)

      - [Statsd Checkpoint Atoms](design-docs/checkpoint-atoms.md) {.tag-contrib}
      - [Perfetto CI](design-docs/continuous-integration.md) {.tag-contrib}
