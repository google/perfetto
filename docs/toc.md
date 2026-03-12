- [Overview](#)

  - [What is Perfetto?](README.md)
  - [What is Tracing?](tracing-101.md)
  - [How do I start using Perfetto?](getting-started/start-using-perfetto.md)

- [For Android](#)

  - [Tutorials](#)

    - [System Tracing](getting-started/system-tracing.md)
    - [Instrumenting with atrace](getting-started/atrace.md)
    - [Memory Profiling](getting-started/memory-profiling.md)
    - [CPU Profiling](getting-started/cpu-profiling.md)

  - [Cookbooks](#)

    - [Analysing Android Traces](getting-started/android-trace-analysis.md)
    - [Periodic Trace Snapshots](getting-started/periodic-trace-snapshots.md)

  - [Case Studies](#)

    - [Debugging Memory Usage](case-studies/memory.md)
    - [Scheduling Blockages](case-studies/scheduling-blockages.md)
    - [Boot Tracing](case-studies/android-boot-tracing.md)
    - [OutOfMemoryError](case-studies/android-outofmemoryerror.md)

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
    - [Batch Trace Processor](analysis/batch-trace-processor.md)
    - [Legacy (v1) Metrics](analysis/metrics.md)
    - [Converting from Perfetto](quickstart/traceconv.md)

  - [Visualization](#)

    - [Perfetto UI](visualization/perfetto-ui.md)
    - [Opening large traces](visualization/large-traces.md)
    - [Deep linking](visualization/deep-linking-to-perfetto-ui.md)
    - [Debug tracks](analysis/debug-tracks.md)

    - [Extending the UI](#)

      - [Overview](visualization/extending-the-ui.md)
      - [Commands and Macros](visualization/ui-automation.md)
      - [Extension Servers](visualization/extension-servers.md)

  - [Reference](#)

    - [Data Sources](#)

      - [CPU Scheduling](data-sources/cpu-scheduling.md)
      - [ATrace](data-sources/atrace.md)
      - [Logcat](data-sources/android-log.md)
      - [Frame Timeline](data-sources/frametimeline.md)
      - [Memory Counters](data-sources/memory-counters.md)
      - [Native Heap Profiler](data-sources/native-heap-profiler.md)
      - [Java Heap Dumps](data-sources/java-heap-profiler.md)
      - [Battery & Power](data-sources/battery-counters.md)
      - [GPU & Game Data](data-sources/android-game-intervention-list.md)

    - [CLI Tools](#)

      - [perfetto](reference/perfetto-cli.md)
      - [traced](reference/traced.md)
      - [traced_probes](reference/traced_probes.md)
      - [heap_profile](reference/heap_profile-cli.md)
      - [tracebox](reference/tracebox.md)

    - [PerfettoSQL](#)

      - [Prelude Tables](analysis/sql-tables.autogen)
      - [Built-in Functions](analysis/builtin.md)
      - [Stats Table](analysis/sql-stats.autogen)

    - [Trace Config Proto](reference/trace-config-proto.autogen)
    - [Trace Packet Proto](reference/trace-packet-proto.autogen)
    - [Synthetic Track Events](reference/synthetic-track-event.md)
    - [Android Version Notes](reference/android-version-notes.md)
    - [Commands Reference](visualization/commands-automation-reference.md)
    - [BigTrace (Single Machine)](deployment/deploying-bigtrace-on-a-single-machine.md)
    - [BigTrace on Kubernetes](deployment/deploying-bigtrace-on-kubernetes.md)

  - [Concepts](#)

    - [Trace Configuration](concepts/config.md)
    - [Buffers and Dataflow](concepts/buffers.md)
    - [Service Model](concepts/service-model.md)
    - [Clock Synchronization](concepts/clock-sync.md)
    - [Concurrent Sessions](concepts/concurrent-tracing-sessions.md)
    - [Detached Mode](concepts/detached-mode.md)
    - [Tracing in Background](learning-more/tracing-in-background.md)
    - [More Android Tracing](learning-more/android.md)

  - [FAQ](faq.md)

- [For Linux](#)

  - [Tutorials](#)

    - [System Tracing](getting-started/system-tracing.md)
    - [Linux ftrace](getting-started/ftrace.md)
    - [CPU Profiling](getting-started/cpu-profiling.md)
    - [Memory Profiling](getting-started/memory-profiling.md)

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
    - [Batch Trace Processor](analysis/batch-trace-processor.md)
    - [Legacy (v1) Metrics](analysis/metrics.md)
    - [Converting from Perfetto](quickstart/traceconv.md)

  - [Visualization](#)

    - [Perfetto UI](visualization/perfetto-ui.md)
    - [Opening Large Traces](visualization/large-traces.md)
    - [Deep Linking](visualization/deep-linking-to-perfetto-ui.md)
    - [Debug Tracks](analysis/debug-tracks.md)
    - [UI Automation](visualization/ui-automation.md)

  - [Reference](#)

    - [Data Sources](#)

      - [CPU Scheduling](data-sources/cpu-scheduling.md)
      - [System Calls](data-sources/syscalls.md)
      - [CPU Frequency](data-sources/cpu-freq.md)
      - [Memory Counters](data-sources/memory-counters.md)
      - [Native Heap Profiler](data-sources/native-heap-profiler.md)

    - [CLI Tools](#)

      - [perfetto](reference/perfetto-cli.md)
      - [traced](reference/traced.md)
      - [traced_probes](reference/traced_probes.md)
      - [heap_profile](reference/heap_profile-cli.md)
      - [tracebox](reference/tracebox.md)

    - [PerfettoSQL](#)

      - [Prelude Tables](analysis/sql-tables.autogen)
      - [Built-in Functions](analysis/builtin.md)
      - [Stats Table](analysis/sql-stats.autogen)

    - [Kernel Track Events](reference/kernel-track-event.md)
    - [Tracing across Reboots](data-sources/previous-boot-trace.md)
    - [Trace Config Proto](reference/trace-config-proto.autogen)
    - [Trace Packet Proto](reference/trace-packet-proto.autogen)
    - [Synthetic Track Events](reference/synthetic-track-event.md)
    - [Commands Reference](visualization/commands-automation-reference.md)
    - [BigTrace (Single Machine)](deployment/deploying-bigtrace-on-a-single-machine.md)
    - [BigTrace on Kubernetes](deployment/deploying-bigtrace-on-kubernetes.md)

  - [Concepts](#)

    - [Trace Configuration](concepts/config.md)
    - [Buffers and Dataflow](concepts/buffers.md)
    - [Service Model](concepts/service-model.md)
    - [Clock Synchronization](concepts/clock-sync.md)
    - [Concurrent Sessions](concepts/concurrent-tracing-sessions.md)
    - [Detached Mode](concepts/detached-mode.md)

  - [FAQ](faq.md)

- [For C/C++](#)

  - [Tutorials](#)

    - [In-App Tracing](getting-started/in-app-tracing.md)

  - [Tracing SDK](#)

    - [Tracing SDK](instrumentation/tracing-sdk.md)
    - [Track Events](instrumentation/track-events.md)
    - [Interceptors](instrumentation/interceptors.md)

  - [Trace Analysis](#)

    - [Commands Automation Reference](visualization/commands-automation-reference.md)
    - [Extension Server Protocol](visualization/extension-server-protocol.md)

  - [Visualization](#)

    - [Perfetto UI](visualization/perfetto-ui.md)
    - [Opening Large Traces](visualization/large-traces.md)
    - [Deep Linking](visualization/deep-linking-to-perfetto-ui.md)
    - [Debug Tracks](analysis/debug-tracks.md)
    - [UI Automation](visualization/ui-automation.md)

  - [Reference](#)

    - [PerfettoSQL](#)

      - [Prelude Tables](analysis/sql-tables.autogen)
      - [Built-in Functions](analysis/builtin.md)
      - [Stats Table](analysis/sql-stats.autogen)

    - [Trace Config Proto](reference/trace-config-proto.autogen)
    - [Trace Packet Proto](reference/trace-packet-proto.autogen)
    - [Synthetic Track Events](reference/synthetic-track-event.md)
    - [Commands Reference](visualization/commands-automation-reference.md)

  - [Concepts](#)

    - [Trace Configuration](concepts/config.md)
    - [Buffers and Dataflow](concepts/buffers.md)
    - [Service Model](concepts/service-model.md)
    - [Clock Synchronization](concepts/clock-sync.md)

  - [FAQ](faq.md)

- [For Chrome](#)

  - [Tutorials](#)

    - [Recording Chrome Traces](getting-started/chrome-tracing.md)

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
    - [Batch Trace Processor](analysis/batch-trace-processor.md)
    - [Legacy (v1) Metrics](analysis/metrics.md)
    - [Converting from Perfetto](quickstart/traceconv.md)

  - [Visualization](#)

    - [Perfetto UI](visualization/perfetto-ui.md)
    - [Opening Large Traces](visualization/large-traces.md)
    - [Deep Linking](visualization/deep-linking-to-perfetto-ui.md)
    - [Debug Tracks](analysis/debug-tracks.md)
    - [UI Automation](visualization/ui-automation.md)

  - [Reference](#)

    - [PerfettoSQL](#)

      - [Prelude Tables](analysis/sql-tables.autogen)
      - [Built-in Functions](analysis/builtin.md)
      - [Stats Table](analysis/sql-stats.autogen)

    - [Trace Config Proto](reference/trace-config-proto.autogen)
    - [Trace Packet Proto](reference/trace-packet-proto.autogen)
    - [Synthetic Track Events](reference/synthetic-track-event.md)
    - [Commands Reference](visualization/commands-automation-reference.md)

  - [Concepts](#)

    - [Trace Configuration](concepts/config.md)
    - [Clock Synchronization](concepts/clock-sync.md)

  - [FAQ](faq.md)

- [For Performance Engineers](#)

  - [Getting Started](#)

    - [Importing Other Formats](getting-started/other-formats.md)
    - [Converting Data to Perfetto](getting-started/converting.md)

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
    - [Batch Trace Processor](analysis/batch-trace-processor.md)
    - [Legacy (v1) Metrics](analysis/metrics.md)
    - [Converting from Perfetto](quickstart/traceconv.md)

  - [Visualization](#)

    - [Perfetto UI](visualization/perfetto-ui.md)
    - [Opening Large Traces](visualization/large-traces.md)
    - [Deep Linking](visualization/deep-linking-to-perfetto-ui.md)
    - [Debug Tracks](analysis/debug-tracks.md)
    - [UI Automation](visualization/ui-automation.md)

  - [Reference](#)

    - [PerfettoSQL](#)

      - [Prelude Tables](analysis/sql-tables.autogen)
      - [Built-in Functions](analysis/builtin.md)
      - [Stats Table](analysis/sql-stats.autogen)

    - [Synthetic Track Events](reference/synthetic-track-event.md)
    - [Trace Config Proto](reference/trace-config-proto.autogen)
    - [Trace Packet Proto](reference/trace-packet-proto.autogen)
    - [Commands Reference](visualization/commands-automation-reference.md)

  - [FAQ](faq.md)

- [For Contributors](#)

  - [Development](#)

    - [Getting Started](contributing/getting-started.md)
    - [Common Tasks](contributing/common-tasks.md)
    - [Building](contributing/build-instructions.md)
    - [Testing](contributing/testing.md)
    - [Developer Tools](contributing/developer-tools.md)
    - [Become a Committer](contributing/become-a-committer.md)

  - [UI Development](#)

    - [Getting Started](contributing/ui-getting-started.md)
    - [Plugins](contributing/ui-plugins.md)

  - [Releases](#)

    - [SDK Release](contributing/sdk-releasing.md)
    - [Python Release](contributing/python-releasing.md)
    - [UI Release](visualization/perfetto-ui-release-process.md)
    - [Chrome Branches](contributing/chrome-branches.md)
    - [SQLite Upgrade](contributing/sqlite-upgrade-guide.md)

  - [Design Documents](#)

    - [API and ABI Surface](design-docs/api-and-abi.md)
    - [Life of a Tracing Session](design-docs/life-of-a-tracing-session.md)
    - [ProtoZero](design-docs/protozero.md)
    - [Security Model](design-docs/security-model.md)
    - [Trace Processor Architecture](design-docs/trace-processor-architecture.md)
    - [Heapprofd Design](design-docs/heapprofd-design.md)
    - [Heapprofd Wire Protocol](design-docs/heapprofd-wire-protocol.md)
    - [Heapprofd Sampling](design-docs/heapprofd-sampling.md)
    - [Batch Trace Processor](design-docs/batch-trace-processor.md)
    - [Statsd Checkpoint Atoms](design-docs/checkpoint-atoms.md)
    - [Perfetto CI](design-docs/continuous-integration.md)
    - [LockFreeTaskRunner](design-docs/lock-free-task-runner.md)
