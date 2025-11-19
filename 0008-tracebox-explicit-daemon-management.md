📄 **RFC Doc:** [0008-tracebox-explicit-daemon-management.md](https://github.com/google/perfetto/blob/rfcs/0008-tracebox-explicit-daemon-management.md)

---

# Tracebox Explicit Daemon Management

**Authors:** @sashwinbalaji, @primiano

**Status:** Implemented

## Problem

`tracebox` is a key tool for using Perfetto, especially on Linux and for
development purposes, bundling `traced`, `traced_probes`, and the `perfetto` CLI
into a single binary. However, its current "autostart" mode—where invoking
`tracebox` with trace arguments automatically spawns daemons—has several
structural issues that create a confusing and unreliable user experience.

1.  **Daemon Lifecycle and SDK Integration:** In its default autostart mode,
    `tracebox` spawns `traced` and `traced_probes` only for the duration of the
    command's execution. This ephemeral nature, coupled with the use of private,
    temporary sockets, creates significant problems:

    *   **Perfetto SDK Users:** Processes instrumented with the Perfetto SDK
        (e.g., using `track_event`) expect a persistent tracing service on
        standard system sockets. When an SDK-instrumented application starts, it
        fails to connect because no daemon is running. If `tracebox` is then
        used to trace, it spawns its own daemons on private sockets (e.g.,
        PID-based abstract sockets on Linux) which the already-running
        application cannot discover. This results in traces missing all SDK data
        and is a major source of developer confusion, as highlighted in issues
        like [#3437](https://github.com/google/perfetto/issues/3437),
        [#2105](https://github.com/google/perfetto/issues/2105), and
        [#850](https://github.com/google/perfetto/issues/850).
    *   **Other Producers:** External tools (e.g., Mesa) that act as trace
        producers also require a stable, accessible tracing service and face the
        same discovery issues.

2.  **User Experience:** It is not obvious to users whether the required daemons
    are running or which sockets are in use. The lack of a persistent service
    model on non-Android platforms is contrary to developer expectations for
    system tracing tools.

3.  **System Integration:** `tracebox` lacks a straightforward way to set up a
    persistent, system-wide tracing service. For users on `systemd`-based
    distributions without a Debian package, there is no simple migration path
    from a temporary setup to a proper service installation. Furthermore, the
    current autostart behavior would conflict with or hide daemons managed by a
    future `apt install` of Perfetto.

4.  **Platform Inconsistency:** The daemon management behavior is inconsistent
    across platforms. On Linux/Android, it uses PID-based abstract domain
    sockets (`@traced-c-PID`) that auto-cleanup, while on macOS it creates
    filesystem sockets (`/tmp/traced-c-PID`) that are left stale after a crash.
    On Windows, the autostart mode is entirely unsupported and triggers a
    `PERFETTO_FATAL`, making any cross-platform workflow unreliable.

5.  **Session Clashes:** Running multiple `tracebox` instances simultaneously
    leads to conflicts as they compete for control of system resources like
    ftrace and the same socket paths. This can result in catastrophic failures
    and undefined behavior, as noted in
    [#2903](https://github.com/google/perfetto/issues/2903).

## Decision

To provide a clearer, more robust, and less mysterious model, `tracebox`'s
daemon management will become explicit. This is a breaking behavioral change.

1.  **Explicit Daemon Control by Default:** Running `tracebox` with trace
    configuration arguments (e.g., `tracebox -c config.pbtx`) will **no longer**
    automatically start `traced` and `traced_probes`. It will require the
    daemons to be already running and accessible on standard system sockets
    (e.g., `/run/perfetto/...` or `/tmp/perfetto-...`).

2.  **New `tracebox ctl` Applet for Daemon Management:** A new applet is
    introduced to manage the lifecycle of daemons for users not relying on a
    system package manager. It supports two distinct modes of operation:

    *   **User-Session Daemons:**
        *   `tracebox ctl start`: Starts `traced` and `traced_probes` as
            detached background daemons **for the current user**. Tries
            `/run/perfetto/` if writable (root or proper permissions), otherwise
            falls back to `/tmp/`. Automatically sets environment variables.
        *   `tracebox ctl stop`: Stops the daemons started via `ctl start`.
        *   `tracebox ctl status`: Reports the status of these daemons.
    *   **System-Wide Service Installation:** *(NOT IMPLEMENTED)*
        *   Removed per feedback - Debian packaging should handle this.
        *   Users should use package manager or run `ctl start` as root.

3.  **Backward Compatibility via Flag:** The current self-contained execution
    model is preserved for existing scripts via the `--autodaemonize` flag
    (simplified from `--autodaemonize=session`).

4.  **Clear Guidance:** If `tracebox` is invoked and daemons are not detected,
    it will exit with an actionable error message, pointing to `systemctl` (for
    system-managed installs) or `tracebox ctl start` (for user-managed daemons).

## Design

### `tracebox` Default Mode Changes

When `tracebox` is invoked without a specific applet name (e.g., `tracebox -c
config.pbtx ...`):

*   **`--autodaemonize`**: This flag preserves the legacy behavior.
    Daemons are spawned on private, temporary sockets (PID-based abstract
    sockets on Linux/Android, `/tmp/traced-*-PID` on macOS) and live only for
    the duration of the command. **Not recommended** for regular use.

*   **Flag omitted (new default)**:
    1.  **Daemon Check:** Attempts non-blocking connection to expected
        socket locations. Search order:
        *   Environment Variables: `PERFETTO_PRODUCER_SOCK_NAME`, `PERFETTO_CONSUMER_SOCK_NAME`
        *   Android system sockets: `/dev/socket/traced_{producer,consumer}`
        *   Systemd/root path: `/run/perfetto/traced-{producer,consumer}.sock`
        *   User-space path: `/tmp/perfetto-{producer,consumer}`
    2.  **On Success:** Uses the first responsive sockets found, **automatically
        sets environment variables**, and proceeds with the trace.
    3.  **On Failure:** Exits with error code and actionable message:
        ```text
        Error: Perfetto tracing daemons (traced, traced_probes) are not running.
        - To run daemons as the current user: `tracebox ctl start`
        - For a self-contained run: `tracebox --autodaemonize ...` (Not Recommended)
        More info at: https://perfetto.dev/docs/reference/tracebox
        ```

### `tracebox ctl` Applet

Provides explicit daemon lifecycle management for user-session daemons:

*   **`tracebox ctl start [--log]`**:
    *   **Systemd Detection:** If systemd service files exist AND running as root,
        uses `systemctl start traced traced-probes` instead of manual daemonization.
        This prevents conflicts with package-managed installations.
    *   **Already Running Check:** Verifies via socket connectivity if daemons
        are already accessible. If so, reports this and exits successfully.
    *   **Runtime Directory Selection:**
        *   Tries `/run/perfetto/` if writable (root or proper permissions)
        *   Falls back to `/tmp/` otherwise
        *   On Android: Always uses `/tmp/` for user-session daemons (system
            daemons use `/dev/socket/`)
    *   **Daemonization:** Uses double-fork to fully detach from terminal
    *   **Socket Naming:**
        *   `/run/perfetto/`: `traced-producer.sock`, `traced-consumer.sock`
        *   `/tmp/`: `perfetto-producer`, `perfetto-consumer` (no .sock extension)
    *   **PID Management:** Stores PID files as `traced.pid`, `traced_probes.pid`
        in the same directory as sockets
    *   **Environment Variables:** Automatically sets `PERFETTO_*_SOCK_NAME`
    *   **Logging (optional):** `--log` flag redirects stdout/stderr to
        `traced.log` and `traced_probes.log` in the runtime directory
    *   **SDK Warning:** If using `/tmp/`, warns that SDK apps may fail to connect

*   **`tracebox ctl stop`**:
    *   Searches for PID files in `/run/perfetto/` and `/tmp/`
    *   Sends SIGTERM to daemon processes
    *   Cleans up PID files
    *   **Systemd Integration:** If no PID files found but daemons are running
        AND systemd service exists AND running as root, uses
        `systemctl stop traced traced-probes`

*   **`tracebox ctl status`**:
    *   Searches for PID files in standard locations
    *   Verifies processes are running via `kill(pid, 0)`
    *   Tests socket connectivity
    *   Cleans up stale PID files
    *   Shows socket paths and daemon status

### Interaction with System Packages

*   **Debian/systemd packages** install units managing daemons with sockets in
    `/run/perfetto/`. This is the canonical method for system-wide installations.
*   **`tracebox ctl start` behavior:**
    *   Detects systemd service files (checks for existence, not running state)
    *   If running as root AND systemd detected: Uses `systemctl start`
    *   If not root AND systemd detected: Instructs user to use `sudo systemctl start`
    *   Otherwise: Starts user-session daemons
*   **Socket Priority:** Both `perfetto` and `tracebox` binaries prioritize
    `/run/perfetto/` sockets over `/tmp/` when auto-discovering.

### Socket Paths Summary

| Management Method             | Producer Socket | Consumer Socket | Persistence |
| ----------------------------- | --------------- | --------------- | ----------- |
| **Android system**            | `/dev/socket/traced_producer` | `/dev/socket/traced_consumer` | Persistent |
| **Systemd (package)**         | `/run/perfetto/traced-producer.sock` | `/run/perfetto/traced-consumer.sock` | Persistent |
| **`ctl start` (root)**        | `/run/perfetto/traced-producer.sock` | `/run/perfetto/traced-consumer.sock` | Persistent |
| **`ctl start` (user)**        | `/tmp/perfetto-producer` | `/tmp/perfetto-consumer` | Persistent |
| **`--autodaemonize`**         | `@traced-p-PID` or `/tmp/traced-p-PID` | `@traced-c-PID` or `/tmp/traced-c-PID` | Ephemeral |

### Platform Support

| Platform | `ctl` Support | `--autodaemonize` | Notes |
| -------- | ------------- | ----------------- | ----- |
| Linux    | ✅ Full       | ✅ Abstract sockets | Systemd integration available |
| Android  | ✅ Full       | ✅ Abstract sockets | System daemons in `/dev/socket/` |
| macOS    | ✅ Full       | ✅ Filesystem sockets | No systemd |
| Windows  | ❌ Not supported | ❌ Not supported | Manual daemon startup required |

## Implementation Details

### Key Changes from Original RFC

1.  **Systemd Detection:** Checks for service file **existence** (not running state)
    per @ribalda feedback. This allows distros to provide service files while
    letting users control when to start them.

2.  **No `install-systemd-units`:** Removed per feedback. Debian packaging
    handles this. Users should use package manager or run `ctl start` as root.

3.  **Automatic Environment Variables:** `GetServiceSockets()` automatically sets
    environment variables when daemons are discovered, eliminating manual setup.

4.  **Logging Support:** Added `--log` flag to `ctl start` for debugging,
    redirecting both stdout and stderr to the same log file for ease of use.

5.  **Improved Error Messages:** Clear, actionable messages guide users to the
    right solution based on their setup (systemd vs user-session).

6.  **Stale PID Cleanup:** `ctl status` automatically removes stale PID files
    when processes are no longer running.

### Code Quality

Following Primiano's review standards:
- **Simple and direct:** No over-engineering, every function serves a purpose
- **Early returns:** Reduced nesting for better readability
- **Clear naming:** Descriptive parameter names (`daemon_name`, `log_file_path`)
- **Lambda usage:** Lambdas only for single-function scope
- **No kernel shadowing:** Let system calls return their own errors

### Resolved Questions

✅ **Naming:** `ctl` (not `systemd`) - supports non-systemd systems
✅ **Systemd detection:** Checks file existence, not running state
✅ **Daemonization:** Double-fork matching `base::Daemonize()` style
✅ **PID management:** PID files as `<binary>.pid` in runtime directory
✅ **Windows support:** Not implemented for `ctl` (clear error message)
✅ **SDK compatibility:** Explicit warnings when using `/tmp/` sockets
✅ **Logging:** Optional `--log` flag for debugging

## Alternatives Considered

1.  **Keep Current Behavior:** Rejected. Fails to solve critical SDK integration
    problems.
2.  **Daemonize Forever Automatically:** Rejected as explicit control is cleaner.
3.  **Time-Limited Daemons (e.g., 1-hour or 24-hour):** Rejected. Too complex
    and "mysterious". Explicit-control model is simpler and more predictable.
4.  **`install-systemd-units` command:** Rejected. Debian packaging should
    handle this. Adds unnecessary complexity.

## Migration Guide

### For Users

**Before (old behavior):**
```bash
tracebox -t 10s -o trace.pftrace sched
# Daemons auto-spawned on private sockets
```

**After (recommended):**
```bash
tracebox ctl start
tracebox -t 10s -o trace.pftrace sched
tracebox ctl stop  # Optional
```

**After (backward compatible):**
```bash
tracebox --autodaemonize -t 10s -o trace.pftrace sched
```

### For SDK Users

**Linux (recommended):**
```bash
sudo tracebox ctl start  # Uses /run/perfetto/ sockets
./my_instrumented_app &
tracebox -t 10s -o trace.pftrace
```

**Android:**
System daemons must be running at `/dev/socket/`. User-session daemons
started via `tracebox ctl start` use `/tmp/` and are not accessible to SDK apps.

## Documentation

Comprehensive documentation added to [`docs/reference/tracebox.md`](docs/reference/tracebox.md):
- Platform-specific behavior sections
- Socket path reference tables
- Troubleshooting guide
- Migration instructions
- FILES section with all socket and PID file paths
