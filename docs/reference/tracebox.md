# TRACEBOX(1)

## NAME

tracebox - all-in-one binary for Perfetto tracing services

## SYNOPSIS

```bash
# Recommended: Persistent daemon workflow
tracebox ctl start
tracebox [PERFETTO_OPTIONS]
tracebox ctl stop

# Self-contained mode
tracebox --autodaemonize [PERFETTO_OPTIONS]

# Applet mode
tracebox [applet_name] [args ...]
```

## DESCRIPTION

`tracebox` bundles all Perfetto tracing services (`traced`, `traced_probes`,
`traced_perf`) and the `perfetto` command-line client into a single binary.

It has three modes of operation:

1.  **Managed Mode (Recommended):** Explicitly manage background daemons via
    `tracebox ctl`. This is required for tracing apps using the Perfetto SDK
    or when multiple traces are recorded in sequence.
2.  **Autodaemonize Mode:** The `--autodaemonize` flag spawns temporary daemons
    for the duration of a single trace.
3.  **Applet Mode:** Behaves like the bundled binary if invoked with its name
    (e.g., `tracebox traced`).

**Key behavior change (2025):** `tracebox` no longer spawns temporary daemons by default.
It expects daemons to be already running. Use `tracebox ctl` to manage them or
`--autodaemonize` for the self-contained mode.

## DAEMON MANAGEMENT (tracebox ctl)

The `ctl` applet manages persistent Perfetto daemons for the current user session.

`tracebox ctl start`
:   Starts the services in the background.

`tracebox ctl stop`
:    Stops daemons started via `ctl start` and cleans up PID files.

`tracebox ctl status`
:    Checks if daemons are running and accessible.

### Examples

**Standard workflow (Recommended)**
Start the services once, then record multiple traces.

```bash
tracebox ctl start
tracebox -t 10s -o trace.pftrace sched freq
tracebox ctl stop
```

**Status check**

```bash
tracebox ctl status
```

### SDK Compatibility

**Linux:** Run `sudo tracebox ctl start` to use `/run/perfetto/` sockets. This is
required for applications using the Perfetto SDK (e.g., `track_event`) to connect.

**Android:** SDK apps require system daemons at `/dev/socket/`. User-session daemons
(started via `tracebox ctl start`) use `/tmp/` and are **not** accessible to SDK apps.

## SELF-CONTAINED MODE (--autodaemonize)

The `--autodaemonize` flag spawns temporary daemons for the duration of a single command.

**Limitations:**
- **SDK Incompatibility:** Apps using the Perfetto SDK cannot connect (uses private sockets).
- **Performance:** Less efficient for repeated tracing due to process spawn overhead.

**When to Use:**
- Quick one-off ftrace debugging.
- Environments where persistent daemons aren't feasible.

```bash
tracebox --autodaemonize -t 10s -o trace.pftrace sched freq
```

## APPLET MODE

Invoke bundled applets directly:

```bash
tracebox [applet_name] [args ...]
```

Available applets: `traced`, `traced_probes`, `traced_perf`, `perfetto`, `trigger_perfetto`, `websocket_bridge`, `ctl`.

```bash
tracebox traced --help
tracebox perfetto -t 10s -o trace.pftrace sched
```

## SEE ALSO

[perfetto(1)](perfetto-cli.md), [traced(1)](traced.md), [traced_probes(1)](traced_probes.md)
