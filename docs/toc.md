- [Getting started](#)

  - [What is Perfetto?](README.md)
  - [What is Tracing?](tracing-101.md)
  - [How do I start using Perfetto?](getting-started/start-using-perfetto.md)

  - [Tutorials](#)

    - [Trace Recording](#)

      - [System Tracing](getting-started/recording/system-tracing.md)
      - [In-App Tracing](getting-started/recording/in-app-tracing.md)
      - [Memory Profiling](getting-started/recording/memory-profiling.md)
      - [CPU Profiling](getting-started/recording/cpu-profiling.md)
      - [Chrome Tracing](getting-started/recording/chrome-tracing.md)

    - [Trace Instrumentation](#)

      - [Perfetto SDK](getting-started/instrumentation/sdk.md)
      - [Android atrace](getting-started/instrumentation/atrace.md)
      - [Linux ftrace](getting-started/instrumentation/ftrace.md)

    - [Custom Analysis & Viz](#)

      - [Non-Perfetto formats](getting-started/adhoc/other-formats.md)
      - [Converting to Perfetto](getting-started/adhoc/converting.md)

  - [Case Studies](#)

    - [Android](#)

      - [Debugging memory usage](case-studies/memory.md)

- [Learning more](#)

  - [Concepts](#)

    - [Trace configuration](concepts/config.md)
    - [Buffers and dataflow](concepts/buffers.md)
    - [Service model](concepts/service-model.md)
    - [Clock synchronization](concepts/clock-sync.md)

  - [Expanding Trace Recording](#)

    - [App + System Tracing](learning-more/trace-recording/app-and-system-tracing.md)
    - [System Tracing on Android](learning-more/trace-recording/android.md)

  - [Trace Analysis](#)

    - [PerfettoSQL Syntax](analysis/perfetto-sql-syntax.md)
    - [PerfettoSQL Standard Library](analysis/stdlib-docs.autogen)
    - [Trace Processor (Python)](analysis/trace-processor-python.md)
    - [Trace Processor (C++)](analysis/trace-processor.md)

  - [Trace Visualization](#)

    - [Perfetto UI](visualization/perfetto-ui.md)
    - [Opening large traces](visualization/large-traces.md)
    - [Deep linking](visualization/deep-linking-to-perfetto-ui.md)
    - [Debug tracks](analysis/debug-tracks.md)

  - [Contributing](#)

    - [Getting started](contributing/getting-started.md)
    - [Common tasks](contributing/common-tasks.md)
    - [UI](#)

      - [Getting started](contributing/ui-getting-started.md)
      - [Plugins](contributing/ui-plugins.md)

- [Diving deep](#)

  - [Data sources](#)

    - [Memory Data sources](#)

      - [Native Heap profiler](data-sources/native-heap-profiler.md)
      - [Java heap dumps](data-sources/java-heap-profiler.md)
      - [Counters and events](data-sources/memory-counters.md)

    - [Ftrace Data Sources](#)

      - [Scheduling events](data-sources/cpu-scheduling.md)
      - [System calls](data-sources/syscalls.md)
      - [Frequency scaling](data-sources/cpu-freq.md)

    - [Android Data Sources](#)

      - [Atrace](data-sources/atrace.md)
      - [Logcat](data-sources/android-log.md)
      - [Frame Timeline](data-sources/frametimeline.md)
      - [Battery counters and rails](data-sources/battery-counters.md)
      - [Other data sources](data-sources/android-game-intervention-list.md)

  - [Trace Format Reference]

    - [Trace Packet Proto](reference/trace-packet-proto.autogen)
    - [Advanced Programmatic Generation](reference/synthetic-track-event.md)

  - [Advanced Trace Recording](#)

    - [Trace Config Proto](reference/trace-config-proto.autogen)
    - [Concurrent tracing sessions](concepts/concurrent-tracing-sessions.md)
    - [Detached mode](concepts/detached-mode.md)

    - [Android](#)

      - [Boot Tracing](case-studies/android-boot-tracing.md)
      - [Android Version Notes](reference/android-version-notes.md)

    - [Command Line Reference](#)

      - [perfetto cmdline](reference/perfetto-cli.md)
      - [heap_profile cmdline](reference/heap_profile-cli.md)

  - [Advanced Trace Analysis](#)

    - [PerfettoSQL](#)

      - [PerfettoSQL Prelude tables](analysis/sql-tables.autogen)
      - [PerfettoSQL Built-ins](analysis/builtin.md)
      - [Stats Table Reference](analysis/sql-stats.autogen)

    - [Single Trace Analysis](#)

      - [Trace-based Metrics](analysis/metrics.md)

    - [Multi Trace Analysis](#)

      - [Batch Trace Processor](analysis/batch-trace-processor.md)
      - [Bigtrace](deployment/deploying-bigtrace-on-a-single-machine.md)
      - [Bigtrace on Kubernetes](deployment/deploying-bigtrace-on-kubernetes.md)

  - [Advanced Perfetto SDK](#)

    - [Interceptors](instrumentation/interceptors.md)

  - [Contributor Reference](#)

    - [Building](contributing/build-instructions.md)
    - [Testing](contributing/testing.md)
    - [Developer tools](contributing/developer-tools.md)

  - [Design documents](#)

    - [Recording](#)

      - [API and ABI surface](design-docs/api-and-abi.md)
      - [Life of a tracing session](design-docs/life-of-a-tracing-session.md)
      - [ProtoZero](design-docs/protozero.md)
      - [Security model](design-docs/security-model.md)
      - [Statsd Checkpoint Atoms](design-docs/checkpoint-atoms.md)

    - [Trace analysis](#)

      - [Batch Trace Processor](design-docs/batch-trace-processor.md)

    - [Heap profiling](#)

      - [Heapprofd design](design-docs/heapprofd-design.md)
      - [Heapprofd wire protocol](design-docs/heapprofd-wire-protocol.md)
      - [Heapprofd sampling](design-docs/heapprofd-sampling.md)

    - [Infra](#)

      - [Perfetto CI](design-docs/continuous-integration.md)

  - [Team documentation](#)

    - [UI release process](visualization/perfetto-ui-release-process.md)
    - [Chrome branches](contributing/chrome-branches.md)
