# TRACEBOX(1)

## NAME

tracebox - all-in-one binary for Perfetto tracing services

## DESCRIPTION

`tracebox` is a bundle containing all the tracing services (`traced`,
`traced_probes`) and the `perfetto` commandline client in one binary.

It can be used either to spawn manually the various subprocess or in "autostart"
mode, which will take care of starting and tearing down the services for you.

## AUTOSTART MODE

If no applet name is specified, `tracebox` will behave like the `perfetto`
command, but will also start `traced` and `traced_probes`.

See [perfetto(1)](perfetto-cli.md) for the documentation of the commandline client.

### Autostart Mode Usage

The autostart mode supports both simple and normal modes of `perfetto`'s
operation, and additionally provides a `--system-sockets` flag.

The general syntax for using `tracebox` in *autostart mode* is as follows:

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

`tracebox` can be used to invoke the bundled applets.

The general syntax for using `tracebox` in *manual mode* is as follows:

```
 tracebox [applet_name] [args ...]
```

The following applets are available:

`traced`
:    The Perfetto tracing service daemon.

`traced_probes`
:    Probes for system-wide tracing (ftrace, /proc pollers).

`traced_perf`
:    Perf-based CPU profiling data source.

`perfetto`
:    The commandline client for controlling tracing sessions.

`trigger_perfetto`
:    A utility to activate triggers for a tracing session.

`websocket_bridge`
:    A bridge for connecting to the tracing service via websockets.
