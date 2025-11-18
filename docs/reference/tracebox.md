# TRACEBOX(1)

## NAME

tracebox - all-in-one binary for Perfetto tracing services

## SYNOPSIS

```bash
# Recommended: Persistent daemon workflow
tracebox ctl start [--log]
tracebox [PERFETTO_OPTIONS]
tracebox ctl stop

# Legacy: Self-contained mode
tracebox --autodaemonize [PERFETTO_OPTIONS]

# Applet mode
tracebox [applet_name] [args ...]
```

## DESCRIPTION

`tracebox` bundles all Perfetto tracing services (`traced`, `traced_probes`, 
`traced_perf`) and the `perfetto` command-line client into a single binary.

**Key behavior change:** As of 2025, `tracebox` requires daemons to be already 
running by default (on Linux/macOS). Use `tracebox ctl` to manage daemons or 
`--autodaemonize` for the legacy self-contained mode.

## DAEMON MANAGEMENT (tracebox ctl)

The `ctl` applet manages persistent Perfetto daemons for the current user.

**Platform support:** Linux, macOS, Android. Not available on Windows.

### Commands

`tracebox ctl start [--log]`
:    Starts `traced` and `traced_probes` as persistent background daemons.
     
     - Checks if daemons are already running
     - Yields to systemd if service files are installed (Linux only)
     - On Android: Uses `/tmp/` for user-session daemons
     - On Linux: Tries `/run/perfetto/` for sockets (SDK compatibility)
     - On Linux: Falls back to `/tmp/` if `/run/perfetto/` not writable
     - Stores PID files for management
     - `--log`: Redirects daemon output to `traced.log` and `traced_probes.log`

`tracebox ctl stop`
:    Stops user-session daemons started via `ctl start`.
     
     - On Android: Searches `/tmp/` for PID files
     - On Linux: Searches `/run/perfetto/` and `/tmp/` for PID files
     - Sends SIGTERM to daemon processes
     - Cleans up PID files

`tracebox ctl status`
:    Shows status of user-session daemons.
     
     - Reports running daemons with PIDs
     - Tests socket connectivity
     - Cleans up stale PID files

### Socket Paths

| Management Method | Producer Socket | Consumer Socket |
|-------------------|-----------------|-----------------|
| Android system | `/dev/socket/traced_producer` | `/dev/socket/traced_consumer` |
| Android `ctl start` | `/tmp/perfetto-producer` | `/tmp/perfetto-consumer` |
| Linux `ctl` (as root) | `/run/perfetto/traced-producer.sock` | `/run/perfetto/traced-consumer.sock` |
| Linux `ctl` (as user) | `/tmp/perfetto-producer` | `/tmp/perfetto-consumer` |
| Systemd package | `/run/perfetto/traced-producer.sock` | `/run/perfetto/traced-consumer.sock` |

### Examples

Start daemons and capture a trace:

```bash
tracebox ctl start
tracebox -t 10s -o trace.pftrace sched freq
tracebox ctl stop  # Optional: daemons can stay running
```

Start daemons with logging enabled:

```bash
tracebox ctl start --log
tracebox -t 10s -o trace.pftrace sched freq
# Logs available at /run/perfetto/traced.log (or /tmp/traced.log)
```

Check daemon status:

```bash
tracebox ctl status
```

### Systemd Integration

If systemd service files are detected, `tracebox ctl start` will:
- As root: Attempt to start via `systemctl`
- As user: Instruct to use `sudo systemctl start traced traced-probes`

This prevents conflicts with package-managed installations.

### SDK Compatibility

For applications using the Perfetto SDK (e.g., `track_event`):

**Linux - Recommended:** Run `tracebox ctl start` as root to use `/run/perfetto/` sockets.

**Android:** SDK apps require system daemons at `/dev/socket/traced_{producer,consumer}`.
User-session daemons started via `tracebox ctl start` use `/tmp/` and are not accessible to SDK apps.

**Alternative (Linux only):** Start daemons before your application:
```bash
tracebox ctl start
./my_instrumented_app &
tracebox -t 10s -o trace.pftrace
```

**Won't work:** Using `--autodaemonize` (uses private sockets).

### Ftrace State

`tracebox ctl start` does NOT automatically reset ftrace state. If you encounter 
ftrace issues, manually run:

```bash
traced_probes --reset-ftrace  # Requires root on most systems
```

## SELF-CONTAINED MODE (--autodaemonize)

**NOT RECOMMENDED for regular use.** Use `tracebox ctl` instead.

The `--autodaemonize` flag spawns temporary daemons for a single trace session.

### Limitations

- SDK-instrumented apps won't connect (uses private, PID-based sockets)
- No persistent service for multiple traces
- Less efficient for repeated tracing
- Daemons terminate when tracing completes

### When to Use

- Quick one-off traces without daemon setup
- Environments where persistent daemons aren't feasible
- Testing or development scenarios

### Platform Support

- **Linux/Android:** Abstract domain sockets (`@traced-c-PID`, `@traced-p-PID`)
- **macOS:** Filesystem sockets (`/tmp/traced-c-PID`, `/tmp/traced-p-PID`)
- **Windows:** Not supported

### Example

```bash
tracebox --autodaemonize -t 10s -o trace.pftrace sched freq
```

## APPLET MODE

Invoke bundled applets directly:

```bash
tracebox [applet_name] [args ...]
```

Available applets:

`traced`
:    The Perfetto tracing service daemon. See [traced(1)](traced.md).

`traced_probes`
:    System-wide tracing probes (ftrace, /proc pollers). See [traced_probes(1)](traced_probes.md).

`traced_perf`
:    Perf-based CPU profiling data source (Linux only).

`perfetto`
:    Command-line client for tracing sessions. See [perfetto(1)](perfetto-cli.md).

`trigger_perfetto`
:    Utility to activate triggers for tracing sessions.

`websocket_bridge`
:    Bridge for connecting to tracing service via WebSockets.

`ctl`
:    Daemon lifecycle management (start/stop/status).

### Examples

```bash
tracebox traced --help
tracebox traced_probes --reset-ftrace
tracebox perfetto -t 10s -o trace.pftrace sched
```

## PLATFORM-SPECIFIC BEHAVIOR

### Android

**Default:** Requires daemons to be running.
- System daemons typically use `/dev/socket/traced_{producer,consumer}`
- `tracebox ctl start` creates user-session daemons in `/tmp/`
- Or use `--autodaemonize` for self-contained mode

**Important:** On Android devices with system-wide Perfetto installed, daemons are
usually already running at `/dev/socket/`. Use `tracebox ctl status` to check.

**Note:** SDK-instrumented apps require system daemons and cannot connect to
user-session daemons started via `tracebox ctl start`.

### Linux/macOS

**Default:** Requires daemons to be running.
- Use `tracebox ctl start` to start persistent daemons
- Or use `--autodaemonize` for self-contained mode

### Windows

**Default:** Always spawns daemons with system sockets.
- No daemon-running check
- No `tracebox ctl` support (use manual daemon startup)
- `--autodaemonize` not supported

## ENVIRONMENT VARIABLES

`PERFETTO_PRODUCER_SOCK_NAME`
:    Override producer socket path.

`PERFETTO_CONSUMER_SOCK_NAME`
:    Override consumer socket path.

### Example

```bash
export PERFETTO_PRODUCER_SOCK_NAME=/custom/producer.sock
export PERFETTO_CONSUMER_SOCK_NAME=/custom/consumer.sock
tracebox -t 10s -o trace.pftrace sched
```

## DEPRECATED FLAGS

`--system-sockets`
:    **Deprecated.** System sockets are now the default behavior.
     This flag is ignored with a warning.

## MIGRATION FROM OLD BEHAVIOR

**Old behavior (before 2025):**
- Default: Auto-spawned temporary daemons with private sockets
- `--system-sockets`: Auto-spawned daemons with system sockets

**New behavior:**
- Default: Requires daemons running (use `tracebox ctl start`)
- `--autodaemonize`: Auto-spawns temporary daemons (old default behavior)

**Migration:**

```bash
# Old:
tracebox -t 10s -o trace.pftrace sched

# New (recommended):
tracebox ctl start
tracebox -t 10s -o trace.pftrace sched

# New (backward compatible):
tracebox --autodaemonize -t 10s -o trace.pftrace sched
```

## TROUBLESHOOTING

### Daemons not detected

```bash
tracebox ctl status  # Check daemon status
ls -la /run/perfetto/  # Check socket files
```

### Permission denied

```bash
# Run as root for /run/perfetto/:
sudo tracebox ctl start

# Or use user-space sockets (SDK apps won't connect):
tracebox ctl start  # Falls back to /tmp
```

### Systemd conflicts

```bash
# If systemd service is installed:
sudo systemctl start traced traced-probes

# Or stop systemd service first:
sudo systemctl stop traced traced-probes
tracebox ctl start
```

### SDK apps not connecting

**Linux:**
- Ensure daemons use `/run/perfetto/` sockets (run `ctl start` as root)
- Don't use `--autodaemonize` for SDK tracing
- Start daemons before launching SDK-instrumented apps

**Android:**
- SDK apps require system daemons at `/dev/socket/traced_{producer,consumer}`
- User-session daemons (via `tracebox ctl start`) are not accessible to SDK apps
- Use system-wide Perfetto daemons for SDK tracing on Android

### Ftrace issues

```bash
# Reset ftrace state manually:
sudo traced_probes --reset-ftrace
```

## FILES

### Android

`/dev/socket/traced_producer`
:    Producer socket (Android system installations).

`/dev/socket/traced_consumer`
:    Consumer socket (Android system installations).

`/tmp/perfetto-producer`
:    Producer socket (when started via `tracebox ctl start`).

`/tmp/perfetto-consumer`
:    Consumer socket (when started via `tracebox ctl start`).

`/tmp/traced.pid`
:    PID file for traced daemon (if started via `ctl`).

`/tmp/traced_probes.pid`
:    PID file for traced_probes daemon (if started via `ctl`).

### Linux

#### When started as root (uses `/run/perfetto/`)

`/run/perfetto/traced-producer.sock`
:    Producer socket for the tracing service.

`/run/perfetto/traced-consumer.sock`
:    Consumer socket for the tracing service.

`/run/perfetto/traced.pid`
:    PID file for traced service.

`/run/perfetto/traced_probes.pid`
:    PID file for traced_probes producer.

`/run/perfetto/traced.log`
:    Log file for traced service (when started with `--log`).

`/run/perfetto/traced_probes.log`
:    Log file for traced_probes producer (when started with `--log`).

#### When started as user (uses `/tmp/`)

`/tmp/perfetto-producer`
:    Producer socket for the tracing service.

`/tmp/perfetto-consumer`
:    Consumer socket for the tracing service.

`/tmp/traced.pid`
:    PID file for traced service.

`/tmp/traced_probes.pid`
:    PID file for traced_probes producer.

`/tmp/traced.log`
:    Log file for traced service (when started with `--log`).

`/tmp/traced_probes.log`
:    Log file for traced_probes producer (when started with `--log`).

## SEE ALSO

[perfetto(1)](perfetto-cli.md), [traced(1)](traced.md), [traced_probes(1)](traced_probes.md)

[SDK Integration Guide](/docs/instrumentation/tracing-sdk.md)
