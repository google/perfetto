# TRACEBOX(1)

## NAME

tracebox - all-in-one binary for Perfetto tracing services

## SYNOPSIS

```bash
# Start daemons, capture trace, stop daemons
tracebox ctl start
tracebox -t 10s -o trace.pftrace sched freq
tracebox ctl stop

# Self-contained mode (legacy)
tracebox --autodaemonize -t 10s -o trace.pftrace sched

# Invoke bundled applets
tracebox [applet_name] [args ...]
```

## DESCRIPTION

`tracebox` bundles the Perfetto tracing service (`traced`), system probes
(`traced_probes`), and the `perfetto` command-line client into a single binary.

**Behavior change (2025):** `tracebox` now requires daemons to be running before
capturing traces. Use `tracebox ctl` to manage daemons, or `--autodaemonize` for
the legacy self-contained mode.

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

## SELF-CONTAINED MODE

The `--autodaemonize` flag spawns temporary daemons for a single trace session.

**Limitations:**
- SDK-instrumented apps won't connect (uses private sockets)
- Inefficient for multiple traces
- Not recommended for regular use

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

## MIGRATION

**Old (before 2025):**
```bash
tracebox -t 10s -o trace.pftrace sched
```

**New (recommended):**
```bash
tracebox ctl start
tracebox -t 10s -o trace.pftrace sched
```

**New (backward compatible):**
```bash
tracebox --autodaemonize -t 10s -o trace.pftrace sched
```

## SEE ALSO

[perfetto(1)](perfetto-cli.md), [traced(1)](traced.md), [traced_probes(1)](traced_probes.md)

[Perfetto Documentation](https://perfetto.dev/docs/)
