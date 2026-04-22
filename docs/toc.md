- [Overview](#)

  - [What is Perfetto?](README.md)
  - [What is Tracing?](tracing-101.md)
  - [How do I start using Perfetto?](getting-started/start-using-perfetto.md)

- [Getting Started](#)

  - [Tutorials](#)

    - [System Tracing](getting-started/system-tracing.md) `android linux`
    - [In-App Tracing](getting-started/in-app-tracing.md) `cpp`
    - [Memory Profiling](getting-started/memory-profiling.md) `android linux`
    - [CPU Profiling](getting-started/cpu-profiling.md) `android linux`
    - [Instrumenting with atrace](getting-started/atrace.md) `android`
    - [Linux ftrace](getting-started/ftrace.md) `linux`
    - [Recording Chrome Traces](getting-started/chrome-tracing.md) `chrome`
    - [Importing Other Formats](getting-started/other-formats.md) `perf`
    - [Converting Data to Perfetto](getting-started/converting.md) `perf`

  - [Cookbooks](#)

    - [Analysing Android Traces](getting-started/android-trace-analysis.md) `android`
    - [Periodic Trace Snapshots](getting-started/periodic-trace-snapshots.md) `android`

  - [Case Studies](#)

    - [Debugging Memory Usage](case-studies/memory.md) `android`
    - [Scheduling Blockages](case-studies/scheduling-blockages.md) `android linux`
    - [Boot Tracing](case-studies/android-boot-tracing.md) `android`
    - [OutOfMemoryError](case-studies/android-outofmemoryerror.md) `android`

  - [Contributing](#)

    - [Getting Started](contributing/getting-started.md) `contrib`
    - [Common Tasks](contributing/common-tasks.md) `contrib`

- [Learning More](#)

  - [Concepts](#)

    - [Trace Configuration](concepts/config.md) `android linux cpp chrome`
    - [Buffers and Dataflow](concepts/buffers.md) `android linux cpp`
    - [Service Model](concepts/service-model.md) `android linux cpp`
    - [Clock Synchronization](concepts/clock-sync.md) `android linux cpp chrome`
    - [Concurrent Sessions](concepts/concurrent-tracing-sessions.md) `android linux cpp`
    - [Tracing in Background](learning-more/tracing-in-background.md) `android linux`
    - [More Android Tracing](learning-more/android.md) `android`
    - [Symbolization and Deobfuscation](learning-more/symbolization.md) `android linux`

  - [Tracing SDK](#)

    - [Tracing SDK](instrumentation/tracing-sdk.md) `cpp`
    - [Track Events](instrumentation/track-events.md) `cpp`

  - [Trace Analysis](#)

    - [Getting Started](analysis/getting-started.md) `android linux cpp chrome perf`
    - [PerfettoSQL Getting Started](analysis/perfetto-sql-getting-started.md) `android linux cpp chrome perf`
    - [PerfettoSQL Syntax](analysis/perfetto-sql-syntax.md) `android linux cpp chrome perf`
    - [PerfettoSQL Style Guide](analysis/style-guide.md) `android linux cpp chrome perf`
    - [PerfettoSQL Backwards Compatibility](analysis/perfetto-sql-backcompat.md) `android linux cpp chrome perf`
    - [Trace Processor (C++)](analysis/trace-processor.md) `android linux cpp chrome perf`
    - [Trace Processor (Python)](analysis/trace-processor-python.md) `android linux cpp chrome perf`
    - [Trace Summarization](analysis/trace-summary.md) `android linux cpp chrome perf`
    - [Converting from Perfetto](quickstart/traceconv.md) `android linux cpp chrome perf`

  - [Visualization](#)

    - [Perfetto UI](visualization/perfetto-ui.md) `android linux cpp chrome perf`
    - [Opening Large Traces](visualization/large-traces.md) `android linux cpp chrome perf`
    - [Deep Linking](visualization/deep-linking-to-perfetto-ui.md) `android linux cpp chrome perf`
    - [Debug Tracks](analysis/debug-tracks.md) `android linux cpp chrome perf`
    - [UI Automation](visualization/ui-automation.md) `android linux cpp chrome perf`
    - [Extending the UI](visualization/extending-the-ui.md) `android linux cpp chrome perf`
    - [Extension Servers](visualization/extension-servers.md) `android linux cpp chrome perf`

  - [UI Development](#)

    - [Getting Started](contributing/ui-getting-started.md) `contrib`
    - [Plugins](contributing/ui-plugins.md) `contrib`

  - [FAQ](faq.md) `android linux cpp chrome perf`

- [Diving Deep](#)

  - [Data Sources](#)

    - [CPU Scheduling](data-sources/cpu-scheduling.md) `android linux`
    - [System Calls](data-sources/syscalls.md) `linux`
    - [CPU Frequency](data-sources/cpu-freq.md) `linux`
    - [ATrace](data-sources/atrace.md) `android`
    - [Logcat](data-sources/android-log.md) `android`
    - [Frame Timeline](data-sources/frametimeline.md) `android`
    - [Memory Counters](data-sources/memory-counters.md) `android linux`
    - [Native Heap Profiler](data-sources/native-heap-profiler.md) `android linux`
    - [Java Heap Dumps](data-sources/java-heap-profiler.md) `android`
    - [Battery & Power](data-sources/battery-counters.md) `android`
    - [GPU & Game Data](data-sources/android-game-intervention-list.md) `android`
    - [Tracing across Reboots](data-sources/previous-boot-trace.md) `linux`

  - [CLI Tools](#)

    - [perfetto](reference/perfetto-cli.md) `android linux`
    - [traced](reference/traced.md) `android linux`
    - [traced_probes](reference/traced_probes.md) `android linux`
    - [heap_profile](reference/heap_profile-cli.md) `android linux`
    - [tracebox](reference/tracebox.md) `android linux`

  - [PerfettoSQL Reference](#)

    - [Standard Library](analysis/stdlib-docs.autogen) `android linux cpp chrome perf`
    - [Prelude Tables](analysis/sql-tables.autogen) `android linux cpp chrome perf`
    - [Built-in Functions](analysis/builtin.md) `android linux cpp chrome perf`
    - [Stats Table](analysis/sql-stats.autogen) `android linux cpp chrome perf`

  - [References](#)

    - [Trace Config Proto](reference/trace-config-proto.autogen) `android linux cpp chrome perf`
    - [Trace Packet Proto](reference/trace-packet-proto.autogen) `android linux cpp chrome perf`
    - [Synthetic Track Events](reference/synthetic-track-event.md) `android linux cpp chrome perf`
    - [Commands Reference](visualization/commands-automation-reference.md) `android linux cpp chrome perf`
    - [Extension Server Protocol](visualization/extension-server-protocol.md) `cpp`
    - [Android Version Notes](reference/android-version-notes.md) `android`
    - [Kernel Track Events](reference/kernel-track-event.md) `linux`

  - [Advanced Recording](#)

    - [Detached Mode](concepts/detached-mode.md) `android`
    - [Interceptors](instrumentation/interceptors.md) `cpp`

  - [Advanced Analysis](#)

    - [Legacy (v1) Metrics](analysis/metrics.md) `android linux cpp chrome perf`
    - [Batch Trace Processor](analysis/batch-trace-processor.md) `android linux perf`
    - [BigTrace (Single Machine)](deployment/deploying-bigtrace-on-a-single-machine.md) `android linux perf`
    - [BigTrace on Kubernetes](deployment/deploying-bigtrace-on-kubernetes.md) `android linux perf`

  - [Contributor Reference](#)

    - [Building](contributing/build-instructions.md) `contrib`
    - [Testing](contributing/testing.md) `contrib`
    - [Developer Tools](contributing/developer-tools.md) `contrib`
    - [Become a Committer](contributing/become-a-committer.md) `contrib`

  - [Releases](#)

    - [SDK Release](contributing/sdk-releasing.md) `contrib`
    - [Python Release](contributing/python-releasing.md) `contrib`
    - [UI Release](visualization/perfetto-ui-release-process.md) `contrib`
    - [Chrome Branches](contributing/chrome-branches.md) `contrib`
    - [SQLite Upgrade](contributing/sqlite-upgrade-guide.md) `contrib`

  - [Design Documents](#)

    - [API and ABI Surface](design-docs/api-and-abi.md) `contrib`
    - [Life of a Tracing Session](design-docs/life-of-a-tracing-session.md) `contrib`
    - [ProtoZero](design-docs/protozero.md) `contrib`
    - [Security Model](design-docs/security-model.md) `contrib`
    - [Trace Processor Architecture](design-docs/trace-processor-architecture.md) `contrib`
    - [Heapprofd Design](design-docs/heapprofd-design.md) `contrib`
    - [Heapprofd Wire Protocol](design-docs/heapprofd-wire-protocol.md) `contrib`
    - [Heapprofd Sampling](design-docs/heapprofd-sampling.md) `contrib`
    - [Batch Trace Processor](design-docs/batch-trace-processor.md) `contrib`
    - [Statsd Checkpoint Atoms](design-docs/checkpoint-atoms.md) `contrib`
    - [Perfetto CI](design-docs/continuous-integration.md) `contrib`
    - [LockFreeTaskRunner](design-docs/lock-free-task-runner.md) `contrib`
