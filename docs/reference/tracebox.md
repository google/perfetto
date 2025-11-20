# TRACEBOX(1)

## NAME

tracebox - all-in-one binary for Perfetto tracing services

## SYNOPSIS

```bash
# Start daemons, capture trace, stop daemons
tracebox ctl start
tracebox -t 10s -o trace.pftrace sched freq
tracebox ctl stop

# Autodaemonize mode (self-contained)
tracebox --autodaemonize -t 10s -o trace.pftrace sched

# Invoke bundled applets
tracebox [applet_name] [args ...]
```

## DESCRIPTION

`tracebox` bundles the Perfetto tracing service (`traced`), system probes
(`traced_probes`), and the `perfetto` command-line client into a single binary.

**Update (2025):** `tracebox` now prefers daemons to be managed explicitly via
`tracebox ctl` for better SDK compatibility and performance. The classic
self-contained behavior is still available via the `--autodaemonize` flag.

## DAEMON MANAGEMENT

### tracebox ctl start [--log]

Starts `traced` and `traced_probes` as persistent background daemons.

- Detects and yields to systemd if installed (Linux only)
- `--log`: Enables logging to `traced.log` and `traced_probes.log`

### tracebox ctl stop

Stops daemons started via `ctl start`.

### tracebox ctl status

Shows daemon status and socket paths.

### Examples

```bash
# Basic workflow
tracebox ctl start
tracebox -t 10s -o trace.pftrace sched freq
tracebox ctl stop

# With logging
tracebox ctl start --log
tracebox -t 10s -o trace.pftrace sched
# Check logs at /run/perfetto/traced.log or /tmp/traced.log

# Check status
tracebox ctl status
```

### Systemd Integration

If systemd service files are detected:
- As root: `ctl start` uses `systemctl start traced traced-probes`
- As user: Instructs to use `sudo systemctl start traced traced-probes`

## AUTODAEMONIZE MODE

The `--autodaemonize` flag (formerly default behavior) spawns temporary daemons for the
duration of a single trace session. This mode uses private, temporary sockets
and is fully self-contained.

**When to use:**
- For quick one-off traces where setup simplicity is prioritized over performance.
- If you cannot or do not want to run persistent background daemons.
- For existing scripts or workflows that expect self-contained execution.

**Limitations:**
- **SDK Incompatibility:** Processes instrumented with the Perfetto SDK (e.g.
  using `track_event`) will generally fail to connect to these temporary daemons
  because they listen on private, unpredictable socket addresses.
- **Inefficiency:** Spawns and kills daemons for every trace, which is slower
  for repeated tracing.
- **Concurrency:** Running multiple instances of `tracebox --autodaemonize`
  concurrently can lead to resource conflicts (e.g. with ftrace).

**Example:**
```bash
tracebox --autodaemonize -t 10s -o trace.pftrace sched freq
```

## APPLET MODE

Invoke bundled applets directly:

```bash
tracebox traced --help
tracebox traced_probes --reset-ftrace
tracebox perfetto -t 10s -o trace.pftrace sched
```

Available applets: `traced`, `traced_probes`, `traced_perf`, `perfetto`,
`trigger_perfetto`, `websocket_bridge`, `ctl`

## ENVIRONMENT VARIABLES

`PERFETTO_PRODUCER_SOCK_NAME`
:    Override producer socket path.

`PERFETTO_CONSUMER_SOCK_NAME`
:    Override consumer socket path.

## TROUBLESHOOTING

### Daemons not running

```bash
tracebox ctl status  # Check daemon status
```

If daemons aren't running:
- Use `tracebox ctl start` to start them
- Or use `--autodaemonize` for self-contained mode

### Permission denied

Run as root or add your user to the `perfetto` group:
```bash
sudo usermod -a -G perfetto $USER
```

### SDK apps not connecting

**Linux:** Run `tracebox ctl start` as root to use `/run/perfetto/` sockets
(required for SDK compatibility).

**Android:** SDK apps require system daemons at `/dev/socket/`. User-session
daemons use `/tmp/` and are not accessible to SDK apps.

### Systemd conflicts

```bash
# Use systemd if installed
sudo systemctl start traced traced-probes

# Or stop systemd first
sudo systemctl stop traced traced-probes
tracebox ctl start
```

## MIGRATION GUIDE (2025)

The default behavior of `tracebox` has changed to require explicit daemon
management. This change resolves long-standing issues with SDK connectivity and
provides a more consistent experience.

### Why the change?
In the old model, `tracebox` would silently spawn "ephemeral" daemons on private
sockets. This often confused users because apps instrumented with the Perfetto
SDK (e.g., Chrome, or your own apps) would fail to connect to these hidden
daemons, resulting in traces missing all userspace data. The new model ensures
daemons run on standard system sockets where SDK apps can find them.

`tracebox` supports two main workflows. Choose the one that best fits your needs.

### Option A: Persistent Daemons (Recommended)
Best for: SDK tracing, repeated tracing, system-wide analysis.

1.  Start daemons once: `tracebox ctl start`
2.  Record traces: `tracebox -t 10s ...`
3.  Stop (optional): `tracebox ctl stop`

### Option B: Autodaemonize Mode
Best for: Quick ftrace-only debugging, scripts requiring self-contained binaries.

1.  Record trace: `tracebox --autodaemonize -t 10s ...`

This mode behaves like older `tracebox` versions, spawning temporary daemons
for the duration of the command.

## SEE ALSO

[perfetto(1)](perfetto-cli.md), [traced(1)](traced.md), [traced_probes(1)](traced_probes.md)

[Perfetto Documentation](https://perfetto.dev/docs/)
