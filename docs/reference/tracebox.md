# TRACEBOX(1)

## NAME

tracebox - all-in-one binary for Perfetto tracing services

## DESCRIPTION

`tracebox` bundles all Perfetto tracing services (`traced`, `traced_probes`,
`traced_perf`) and the `perfetto` command-line client into a single binary. It
is the primary tool for recording traces on Linux systems where Perfetto is not
pre-installed as a system service.

**Key behavior change (2025):** `tracebox` no longer spawns temporary daemons by default.
It expects daemons to be already running. Use `tracebox ctl` to manage them or
`--autodaemonize` for the classic self-contained mode.

## MODES OF OPERATION

`tracebox` supports three distinct modes of operation depending on how you want
to manage the lifecycle of the tracing services.

### 1. Managed Mode (Recommended)

In this mode, you explicitly start and stop the tracing services using the `ctl`
applet. This creates persistent background daemons that remain active across
multiple tracing sessions.

**Commands:**

`tracebox ctl start`
:   Starts `traced` and `traced_probes` in the background. On Linux, running
    as root (via `sudo`) is highly recommended to place sockets in the standard
    `/run/perfetto/` directory.

`tracebox ctl stop`
:   Stops daemons started via `ctl start` and cleans up PID files.

`tracebox ctl status`
:   Checks if daemons are running, accessible, and which sockets they are using.

**Example Workflow:**

```bash
# Start services once (as root for full system/SDK support)
sudo ./tracebox ctl start

# Record multiple traces
sudo ./tracebox -t 10s -o trace1.pftrace sched
sudo ./tracebox -t 10s -o trace2.pftrace sched

# Stop services when finished
sudo ./tracebox ctl stop
```

### 2. Autodaemonize Mode

In this mode, `tracebox` spawns temporary, ephemeral daemons solely for the
duration of a single command. The daemons are cleaned up automatically when the
command finishes.

**Usage:**

Pass the `--autodaemonize` flag before other arguments.

```bash
# Start daemons, record trace, stop daemons
sudo ./tracebox --autodaemonize -t 10s -o trace.pftrace sched
```

### 3. Applet Mode

`tracebox` can behave like any of the bundled binaries if invoked with that
binary's name as the first argument (or if symlinked to that name).

**Available Applets:** `traced`, `traced_probes`, `traced_perf`, `perfetto`,
`trigger_perfetto`, `websocket_bridge`, `ctl`.

**Example:**

```bash
# equivalent to running the standalone 'perfetto' client
./tracebox perfetto -t 10s -o trace.pftrace sched
```

## CHOOSING BETWEEN MANAGED AND AUTODAEMONIZE

Choosing the right mode depends on your specific tracing needs, particularly
regarding application instrumentation and workflow frequency.

### When to use Managed Mode (`ctl`)

This is the preferred mode for most workflows.

*   **SDK/App Tracing:** If you are tracing applications instrumented with the
    Perfetto SDK (using `track_event`), you **must** use Managed Mode (usually
    as root). SDK applications only connect to the standard system socket paths
    (e.g., `/run/perfetto/` on Linux).
*   **Repeated Tracing:** If you are recording multiple traces in succession,
    Managed Mode avoids the overhead of restarting the services for every trace.
*   **Interactivity:** Useful when manually exploring system behavior and you
    want the tracing service to be always available.

### When to use Autodaemonize Mode

*   **One-off Scripts:** Useful for self-contained scripts or cron jobs where
    you want to ensure no leftover processes remain after execution.
*   **Debugging:** Quick verification of ftrace events where setting up a full
    service is unnecessary.
*   **Limitations:** This mode uses private, randomized socket paths (e.g., in
    `/tmp/`). Applications using the Perfetto SDK **cannot** connect to these
    daemons, so you will not capture userspace instrumentation.
