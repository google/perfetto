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
tracebox --legacy -t 10s -o trace.pftrace sched

# Invoke bundled applets
tracebox [applet_name] [args ...]
```

## DESCRIPTION

`tracebox` bundles the Perfetto tracing service (`traced`), system probes
(`traced_probes`), and the `perfetto` command-line client into a single binary.

**Behavior change (2025):** `tracebox` now requires daemons to be running before
capturing traces. Use `tracebox ctl` to manage daemons, or `--legacy` for
the legacy self-contained mode.

If you are a user of `tracebox` in its default mode (i.e., you don't use
`--legacy`), you should read the migration guide below. The new default
behavior provides a more robust and predictable experience, especially for
processes instrumented with the Perfetto SDK, but it requires a small change to
your workflow.

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

## LEGACY SELF-CONTAINED MODE

The `--legacy` flag preserves the legacy behavior of `tracebox` where it
spawns temporary daemons for the duration of a single trace session. This mode
uses private, temporary sockets and is fully self-contained.

**When to use:**
- For quick one-off traces where you don't want to manage daemons.
- If you cannot or do not want to run persistent background daemons.
- For scripts that rely on the old behavior and haven't been migrated yet.

**Limitations:**
- **SDK Incompatibility:** Processes instrumented with the Perfetto SDK (e.g.
  using `track_event`) will generally fail to connect to these temporary daemons
  because they listen on private, unpredictable socket addresses.
- **Inefficiency:** Spawns and kills daemons for every trace, which is slower
  for repeated tracing.
- **Concurrency:** Running multiple instances of `tracebox --legacy`
  concurrently can lead to resource conflicts (e.g. with ftrace).

**Example:**
```bash
tracebox --legacy -t 10s -o trace.pftrace sched freq
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
- Or use `--legacy` for self-contained mode

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

### Migration Steps

**Scenario 1: You run interactive traces manually**

*   **Old way:**
    ```bash
    tracebox -t 10s -o trace.pftrace sched
    ```
*   **New way (Recommended):**
    Run this once per boot (or when needed):
    ```bash
    tracebox ctl start
    ```
    Then run your traces as usual:
    ```bash
    tracebox -t 10s -o trace.pftrace sched
    ```
    When done (optional):
    ```bash
    tracebox ctl stop
    ```

**Scenario 2: You have scripts that wrap tracebox**

*   **Option A (Better):** Update your scripts to ensure daemons are running
    using `tracebox ctl start` before tracing.
*   **Option B (Faster fix):** Update your scripts to use the `--legacy`
    flag to restore the old behavior.
    ```bash
    tracebox --legacy -t 10s -o trace.pftrace sched
    ```

**Scenario 3: You are tracing SDK-instrumented apps**

You **must** use the new explicit daemon mode. The legacy `--legacy` mode
will likely not work because your app won't be able to discover the temporary
sockets.

## SEE ALSO

[perfetto(1)](perfetto-cli.md), [traced(1)](traced.md), [traced_probes(1)](traced_probes.md)

[Perfetto Documentation](https://perfetto.dev/docs/)
