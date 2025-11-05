# Tracebox Explicit Daemon Management

**Authors:** @sashwinbalaji, @primiano

**Status:** Draft

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
    (e.g., `/run/perfetto/...` or `/tmp/perfetto/...`).

2.  **New `tracebox ctl` Applet for Daemon Management:** A new applet is
    introduced to manage the lifecycle of daemons for users not relying on a
    system package manager. It supports two distinct modes of operation:

    *   **User-Session Daemons:**
        *   `tracebox ctl start`: Starts `traced` and `traced_probes` as
            detached background daemons **for the current user**, using sockets
            in `/tmp/`. This is for temporary, non-system-wide use.
        *   `tracebox ctl stop`: Stops the daemons started via `ctl start`.
        *   `tracebox ctl status`: Reports the status of these daemons.
    *   **System-Wide Service Installation:**
        *   `tracebox ctl install-systemd-units [--start]`: For users on
            `systemd`-based systems, this command generates and installs
            `systemd` unit files, effectively replicating a system package
            installation. This is a one-time setup action for a persistent,
            system-wide service.

3.  **Backward Compatibility via Flag:** The current self-contained execution
    model is preserved for existing scripts via the `--autodaemonize=session`
    flag.

4.  **Clear Guidance:** If `tracebox` is invoked and daemons are not detected,
    it will exit with an actionable error message, pointing to `systemctl` (for
    system-managed installs) or `tracebox ctl start` (for user-managed daemons).

## Design

### `tracebox` Default Mode Changes

When `tracebox` is invoked without a specific applet name (e.g., `tracebox -c
config.pbtx ...`):

*   **`--autodaemonize=session`**: This flag preserves the current behavior.
    Daemons are spawned on private, temporary sockets (PID-based abstract
    sockets on Linux/Android, `/tmp/traced-*-PID` on macOS) and live only for
    the duration of the command.
*   **`--autodaemonize=none` (or flag omitted)**: This is the new default
    behavior.
    1.  **Daemon Check:** It attempts a non-blocking connection to the expected
        socket locations to find active daemons. The search order is:
        *   Environment Variables: `PERFETTO_*_SOCK_NAME`
        *   Systemd path: `/run/perfetto/traced-producer.sock`
        *   User-space path: `/tmp/perfetto-producer.sock`
    2.  **On Success:** It uses the first responsive sockets found and proceeds
        with the trace.
    3.  **On Failure:** It exits with an error code and prints a clear
        message:
        ```text
        Error: Perfetto tracing daemons (traced, traced_probes) are not running.
        - If using a system package (e.g., Debian): `sudo systemctl start perfetto.svc`
        - To run daemons as the current user: `tracebox ctl start`
        - For a self-contained run: `tracebox --autodaemonize=session ...`
        See [link to docs] for more information.
        ```

### `tracebox ctl` Applet

This applet provides two distinct daemon management workflows:

*   **User-Session Management:**

    *   **`tracebox ctl start`**:
    *   Refuses to run if a systemd service for Perfetto is detected (e.g., by
        checking for `/lib/systemd/system/perfetto.svc`), guiding the user to
        `systemctl`.
    *   Checks if daemons are already running on the user-space sockets
        (`/tmp/perfetto-*.sock`). If so, it reports this and exits.
    *   Spawns `traced` and `traced_probes` as detached processes owned by the
        current user, using standard daemonization techniques (e.g., double
        fork).
    *   Uses socket paths in `/tmp/` (e.g.,
        `/tmp/perfetto-{producer,consumer}.sock`) with permissions for the
        current user.
    *   Stores PID files (e.g., in `/tmp/`) for management.
    *   **`tracebox ctl stop`**: Finds the daemons via their PID files and sends
        a termination signal.
    *   **`tracebox ctl status`**: Checks if the user-session daemons are
        responsive on the `/tmp/` sockets.

*   **System-Wide Service Installation:**

    *   **`tracebox ctl install-systemd-units`**: This command (requiring
        `sudo`) generates `traced.service`, `traced_probes.service`, and a
        `perfetto.svc` target and installs them into `/etc/systemd/system/`.
        This provides a persistent setup that survives reboots.
    *   The optional `--start` flag will also execute `systemctl daemon-reload`
        and `systemctl start perfetto.svc`.
    *   This command should refuse to run if package-managed unit files are
        already detected (e.g., in `/lib/systemd/system/`).

### Interaction with Debian Package

*   A Debian package will install `systemd` units to manage daemons using
    sockets in `/run/perfetto/`. This is the canonical method.
*   The `perfetto` and `tracebox` binaries will prioritize connecting to the
    `/run/perfetto/` sockets.
*   The `tracebox ctl start` and `tracebox ctl install-systemd-units` commands
    will yield to a Debian package installation to prevent conflicts.

### Socket Paths Summary

| Management Method             | Producer/Consumer Socket Paths              | Persistence |
| ----------------------------- | ------------------------------------------- | ----------- |
| **Systemd (Debian)**          | `/run/perfetto/{producer,consumer}.sock`    | Persistent  |
| **`tracebox ctl`**            | `/tmp/perfetto-{producer,consumer}.sock`    | Persistent  |
| **`--autodaemonize=session`** | Abstract sockets or `/tmp/traced-{c,p}-PID` | Ephemeral   |

### SDK Enhancements

*   While out of scope for this RFC, the client library's reconnection logic
    could be enhanced to use mechanisms like `inotify` on Linux to detect socket
    creation and trigger an immediate reconnection attempt.
    ([WIP CL](https://github.com/google/perfetto/pull/2931))

## Alternatives Considered

1.  **Keep Current Behavior:** Rejected. Fails to solve critical SDK integration
    problems.
2.  **Daemonize Forever Automatically:** Rejected as explicit control is more
    cleaner.
3.  **Time-Limited Daemons (e.g., 1-hour or 24-hour):** Rejected. This approach
    was deemed too complex and "mysterious". Reliably managing timeouts,
    handling edge cases with active tracing sessions, and debugging issues
    across platforms would introduce significant maintenance overhead. The
    explicit-control model is simpler and more predictable.

## Open Questions

*   Do we really need `install-systemd-units` option as if Debian packaging goes
    well that should cover most of the cases and mixing both can lead to
    confusion.
*   What is the appropriate mechanism for `tracebox ctl` commands to detect a
    system-wide Perfetto installation to yield control to `systemctl`?
*   Detailed implementation of daemonization and PID management for `ctl stop`
    on all supported platforms (Linux, Mac, Windows).
