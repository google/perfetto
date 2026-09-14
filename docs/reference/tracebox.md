# TRACEBOX(1)

## NAME

tracebox - all-in-one binary for Perfetto tracing services

## DESCRIPTION

`tracebox` is a bundle containing all the tracing services (`traced`,
`traced_probes`) and the `perfetto` command-line client in one binary.

You can spawn the subprocesses manually or use "autostart" mode to start and
stop the services for you.

## AUTOSTART MODE

If you omit the applet name, `tracebox` behaves like the `perfetto` command and
also starts `traced` and `traced_probes`.

See [perfetto(1)](perfetto-cli.md) for the command-line client documentation.

### Autostart Mode Usage

The autostart mode supports both simple and normal modes of `perfetto`'s
operation and also provides a `--system-sockets` flag.

Use this syntax for `tracebox` in *autostart mode*:

```
 tracebox [PERFETTO_OPTIONS] [TRACEBOX_OPTIONS] [EVENT_SPECIFIERS]
```

`--system-sockets`
:    Forces the use of system-sockets when using autostart mode.
     By default, `tracebox` uses a private socket namespace to avoid
     conflicts with system-wide `traced` daemons. This flag forces it to
     use the standard system sockets, which is useful for debugging
     interactions with the system `traced` service.

#### Simple Mode Example

To capture a 10-second trace of `sched/sched_switch` events in autostart mode:

```bash
tracebox -t 10s -o trace_file.perfetto-trace sched/sched_switch
```

#### Normal Mode Example

To capture a trace using a custom configuration file in autostart mode:

```bash
cat <<EOF > config.pbtx
duration_ms: 5000
buffers {
  size_kb: 1024
  fill_policy: RING_BUFFER
}
data_sources {
  config {
    name: "linux.ftrace"
    ftrace_config {
      ftrace_events: "sched/sched_switch"
    }
  }
}
EOF

tracebox -c config.pbtx --txt -o custom_trace.perfetto-trace
```

## MANUAL MODE

Use `tracebox` to invoke the bundled applets.

Use this syntax for `tracebox` in *manual mode*:

```
 tracebox [applet_name] [args ...]
```

The following applets are available:

`traced`
:    The Perfetto tracing service daemon.

`traced_probes`
:    Probes for system-wide tracing (ftrace, /proc pollers).

`traced_relay`
:    Relays trace data to a remote tracing service. Used to extend a tracing
     session across machines; see
     [Multi-machine architecture](/docs/deployment/multi-machine-architecture.md)
     for the design and
     [Multi-machine recording](/docs/learning-more/multi-machine-tracing.md)
     for setup.

`traced_perf`
:    Perf-based CPU profiling data source.

`perfetto`
:    The command-line client for controlling tracing sessions.

`trigger_perfetto`
:    A utility to activate triggers for a tracing session.

`websocket_bridge`
:    A bridge for connecting to the tracing service via websockets.
