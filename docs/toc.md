- [Getting started](#)

  - [What is Perfetto?](README.md)
  - [What is Tracing?](tracing-101.md)
  - [How do I start using Perfetto?](getting-started/start-using-perfetto.md)

  - [Tutorials](#)

    - [Full-Stack Perfetto](#)

      - [System Tracing](getting-started/system-tracing.md)
      - [In-App Tracing](getting-started/in-app-tracing.md)
      - [Memory Profiling](getting-started/memory-profiling.md)
      - [CPU Profiling](getting-started/cpu-profiling.md)

    - [Adding Tracepoints](#)

      - [Android atrace](getting-started/atrace.md)
      - [Linux ftrace](getting-started/ftrace.md)

    - [Non-Perfetto Trace Analysis](#)

      - [Supported trace formats](getting-started/other-formats.md)
      - [Converting to Perfetto](getting-started/converting.md)

  - [Cookbooks](#)

    - [Analysing Android Traces](getting-started/android-trace-analysis.md)

  - [Case Studies](#)

    - [Android Memory Usage](case-studies/memory.md)
    - [Scheduling blockages](case-studies/scheduling-blockages.md)

- [Learning more](#)

  - [Concepts](#)

    - [Trace configuration](concepts/config.md)
    - [Buffers and dataflow](concepts/buffers.md)
    - [Service model](concepts/service-model.md)
    - [Clock synchronization](concepts/clock-sync.md)

  - [Trace Recording](#)

    - [Tracing in Background](learning-more/tracing-in-background.md)
    - [More Android tracing](learning-more/android.md)
    - [Chrome Tracing](getting-started/chrome-tracing.md)

  - [Trace Instrumentation](#)

    - [Tracing SDK](instrumentation/tracing-sdk.md)
    - [Track Event](instrumentation/track-events.md)

  - [Trace Analysis](#)

    - [Getting Started](analysis/getting-started.md)
    - [PerfettoSQL](#)
      - [Getting Started](analysis/perfetto-sql-getting-started.md)
      - [Standard Library](analysis/stdlib-docs.autogen)
      - [Syntax](analysis/perfetto-sql-syntax.md)
      - [Style Guide](analysis/style-guide.md)
      - [Backwards Compatibility](analysis/perfetto-sql-backcompat.md)
    - [Trace Processor](#)
      - [Trace Processor (C++)](analysis/trace-processor.md)
      - [Trace Processor (Python)](analysis/trace-processor-python.md)
    - [Trace Summarization](analysis/trace-summary.md)
    - [Converting from Perfetto](quickstart/traceconv.md)

  - [Trace Visualization](#)

    - [Perfetto UI](visualization/perfetto-ui.md)
    - [Opening large traces](visualization/large-traces.md)
    - [Deep linking](visualization/deep-linking-to-perfetto-ui.md)
    - [Debug tracks](analysis/debug-tracks.md)
    - [UI Automation](visualization/ui-automation.md)

  - [Contributing](#)

    - [Getting started](contributing/getting-started.md)
    - [Common tasks](contributing/common-tasks.md)
    - [Become a committer](contributing/become-a-committer.md)
    - [UI](#)

      - [Getting started](contributing/ui-getting-started.md)
      - [Plugins](contributing/ui-plugins.md)

  - [FAQ](faq.md)

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

  - [Trace Format Reference](#)

    - [Trace Packet Proto](reference/trace-packet-proto.autogen)
    - [Advanced Programmatic Generation](reference/synthetic-track-event.md)

  - [Advanced Trace Recording](#)

    - [Trace Config Proto](reference/trace-config-proto.autogen)
    - [Concurrent tracing sessions](concepts/concurrent-tracing-sessions.md)
    - [Detached mode](concepts/detached-mode.md)

    - [Android](#)

      - [Boot Tracing](case-studies/android-boot-tracing.md)
      - [OutOfMemoryError](case-studies/android-outofmemoryerror.md)
      - [Android Version Notes](reference/android-version-notes.md)

    - [Linux](#)

      - [Kernel track events](reference/kernel-track-event.md)
      - [Tracing across reboots](data-sources/previous-boot-trace.md)

    - [Command Line Reference](#)

      - [perfetto_cmd](reference/perfetto-cli.md)
      - [traced](reference/traced.md)
      - [traced_probes](reference/traced_probes.md)
      - [heap_profile cmdline](reference/heap_profile-cli.md)
      - [tracebox](reference/tracebox.md)

  - [Advanced Trace Analysis](#)

    - [PerfettoSQL](#)

      - [Prelude tables](analysis/sql-tables.autogen)
      - [Built-ins](analysis/builtin.md)
      - [Stats Table Reference](analysis/sql-stats.autogen)

    - [Single Trace Analysis](#)

      - [Legacy (v1) Metrics](analysis/metrics.md)

    - [Multi Trace Analysis](#)

      - [Batch Trace Processor](analysis/batch-trace-processor.md)
      - [Bigtrace](deployment/deploying-bigtrace-on-a-single-machine.md)
      - [Bigtrace on Kubernetes](deployment/deploying-bigtrace-on-kubernetes.md)

  - [Advanced Perfetto SDK](#)

    - [Interceptors](instrumentation/interceptors.md)

  - [Advanced Trace Visualization](#)

    - [Commands Automation Reference](visualization/commands-automation-reference.md)

  - [Contributor Reference](#)

    - [Building](contributing/build-instructions.md)
    - [Testing](contributing/testing.md)
    - [Developer tools](contributing/developer-tools.md)

  - [Team documentation](#)

    - [SDK release process](contributing/sdk-releasing.md)
    - [Python release process](contributing/python-releasing.md)
    - [UI release process](visualization/perfetto-ui-release-process.md)
    - [Chrome branches](contributing/chrome-branches.md)
    - [SQLite upgrade guide](contributing/sqlite-upgrade-guide.md)

    - [Design documents](#)
      - [API and ABI surface](design-docs/api-and-abi.md)
      - [Life of a tracing session](design-docs/life-of-a-tracing-session.md)
      - [ProtoZero](design-docs/protozero.md)
      - [Security model](design-docs/security-model.md)
      - [Statsd Checkpoint Atoms](design-docs/checkpoint-atoms.md)
      - [Batch Trace Processor](design-docs/batch-trace-processor.md)
      - [Trace Processor Architecture](design-docs/trace-processor-architecture.md)
      - [Heapprofd design](design-docs/heapprofd-design.md)
      - [Heapprofd wire protocol](design-docs/heapprofd-wire-protocol.md)
      - [Heapprofd sampling](design-docs/heapprofd-sampling.md)
      - [Perfetto CI](design-docs/continuous-integration.md)
      - [LockFreeTaskRunner](design-docs/lock-free-task-runner.md)

