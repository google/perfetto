# Statsd Checkpoint Atoms
## Tracing

This diagram gives the atoms and the state transitions between when tracing/
All atoms above log the UUID of the trace;
`PERFETTO_TRACED_TRIGGER_STOP_TRACING` is special as it *also* logs the trigger
name which caused trace finalization.

NOTE: dotted lines indicate these transitions only happen in background
configs; transitions with solid lines happen in both background and
non-background cases.

NOTE: for background traces, *either* start triggers or stop triggers are
supported; both cannot happen for the same trace.

```mermaid
graph TD;
    PERFETTO_CMD_TRACE_BEGIN-->PERFETTO_CMD_ON_CONNECT;
    PERFETTO_CMD_BACKGROUND_TRACE_BEGIN-.->PERFETTO_CMD_ON_CONNECT
    PERFETTO_CMD_ON_CONNECT-->PERFETTO_TRACED_ENABLE_TRACING
    PERFETTO_TRACED_ENABLE_TRACING-->PERFETTO_TRACED_START_TRACING
    PERFETTO_TRACED_ENABLE_TRACING-.->|start trigger background traces only|PERFETTO_TRACED_TRIGGER_START_TRACING
    PERFETTO_TRACED_TRIGGER_START_TRACING-.->PERFETTO_TRACED_START_TRACING
    PERFETTO_TRACED_START_TRACING-.->|stop trigger background traces only|PERFETTO_TRACED_TRIGGER_STOP_TRACING
    PERFETTO_TRACED_TRIGGER_STOP_TRACING-.->PERFETTO_TRACED_DISABLE_TRACING
    PERFETTO_TRACED_START_TRACING-->PERFETTO_TRACED_DISABLE_TRACING
    PERFETTO_TRACED_DISABLE_TRACING-->PERFETTO_TRACED_NOTIFY_TRACING_DISABLED
    PERFETTO_TRACED_NOTIFY_TRACING_DISABLED-->PERFETTO_CMD_ON_TRACING_DISABLED
    PERFETTO_CMD_ON_TRACING_DISABLED-->PERFETTO_CMD_FINALIZE_TRACE_AND_EXIT
    PERFETTO_CMD_FINALIZE_TRACE_AND_EXIT-->PERFETTO_CMD_UPLOAD_INCIDENT
    PERFETTO_CMD_FINALIZE_TRACE_AND_EXIT-.->|only if no trigger happened|PERFETTO_CMD_NOT_UPLOADING_EMPTY_TRACE
```

## Triggers

This diagram gives the atoms which can trigger finalization of a trace. 
These atoms will not be reported individually but instead aggregated by trigger name
and reported as a count.

```mermaid
graph TD;
    PERFETTO_CMD_TRIGGER
    PERFETTO_TRIGGER_PERFETTO_TRIGGER
```

