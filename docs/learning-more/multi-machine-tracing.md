# Multi-machine recording

This document describes how to record a single Perfetto trace that captures
events from two Linux machines simultaneously. It uses `traced_relay` on
the second machine to forward producer IPC to a `traced` running on the
first machine.

For background on what multi-machine tracing is and how it works under the
hood, see
[Multi-machine architecture](/docs/deployment/multi-machine-architecture.md).

## Use case

You have a workload split across two Linux machines — e.g. a client on
machine A driving a server on machine B, or a host running a Linux VM —
and you want a single trace covering both, so cross-machine causality is
visible in one timeline and queryable in one trace file.

In the rest of this guide, `host` is the machine that will run `traced`
and own the trace buffers, and `guest` is the second machine whose
producers feed into the same trace via `traced_relay`. Substitute
`<host-ip>` with the IP address (or hostname) of `host` as reachable from
`guest`.

## Prerequisites

* `tracebox` available on both machines. See
  [Start Using Perfetto](/docs/getting-started/start-using-perfetto.md) for
  how to obtain a binary.
* A network path from `guest` to `host` on a chosen TCP port (e.g. port
  `20001`). If there's a firewall between them, open the port.
* No `traced` already running on either machine. On the `guest`, `traced`
  and `traced_relay` would contest the same local producer socket; on the
  `host` you want the `traced` you start below, not a system one.
* `host` and `guest` are separate OS images — two machines, a host plus a
  VM, etc. Pointing both producers at the same kernel does not work.

NOTE: This guide records ftrace events for the example, which on Linux
typically requires running the producer commands as root (or with
`CAP_SYS_ADMIN`). The IPC commands themselves do not require root.

## Usage

### Step 1: Start `traced` on the host, listening on TCP

On `host`:

```bash
PERFETTO_PRODUCER_SOCK_NAME=0.0.0.0:20001 \
  tracebox traced --enable-relay-endpoint
```

`PERFETTO_PRODUCER_SOCK_NAME` rebinds the producer socket from the default
UNIX path to a TCP listener that remote machines can reach.
`--enable-relay-endpoint` makes that socket accept `traced_relay`
connections in addition to ordinary local producers.

Leave this process running.

### Step 2: Start `traced_probes` on the host

In a second shell on `host`:

```bash
PERFETTO_PRODUCER_SOCK_NAME=127.0.0.1:20001 \
  sudo -E tracebox traced_probes
```

The same env var that rebound `traced`'s listener also tells local
producers where to connect — without it, `traced_probes` would still try
the default UNIX socket and fail. `sudo -E` preserves the env var across
the privilege escalation needed for ftrace.

### Step 3: Start `traced_relay` on the guest

On `guest`:

```bash
PERFETTO_RELAY_SOCK_NAME=<host-ip>:20001 \
  tracebox traced_relay
```

`traced_relay` opens the standard local producer socket on `guest` and
forwards every producer IPC frame to the host's relay endpoint. You should
see a startup line of the form:

```
Started traced_relay, listening on /tmp/perfetto-producer, forwarding to <host-ip>:20001
```

(The listening path may instead be `/run/perfetto/traced-producer.sock` if
that directory exists — both are valid Linux defaults.)

Leave this process running.

### Step 4: Start `traced_probes` on the guest

In a second shell on `guest`:

```bash
sudo tracebox traced_probes
```

No env var is needed: with `PERFETTO_PRODUCER_SOCK_NAME` unset,
`traced_probes` connects to the default Linux producer socket — which is
exactly the path `traced_relay` is listening on — so the two find each
other automatically.

### Step 5: Record a trace from the host

Multi-machine tracing requires an explicit `TraceConfig` — the
`tracebox perfetto -t 10s ... sched/sched_switch` shorthand records on
the host machine only (see
[Multi-machine architecture](/docs/deployment/multi-machine-architecture.md#data-source-dispatch)).

On `host`, write a config file:

```bash
cat > config.pbtx <<'EOF'
buffers {
  size_kb: 32768
  fill_policy: RING_BUFFER
}
trace_all_machines: true
data_sources {
  config {
    name: "linux.ftrace"
    ftrace_config {
      ftrace_events: "sched/sched_switch"
    }
  }
}
duration_ms: 10000
EOF
```

Then record:

```bash
tracebox perfetto --txt -c config.pbtx -o trace.pftrace
```

### Step 6: Verify both machines are in the trace

Open `trace.pftrace` at <https://ui.perfetto.dev>. In the SQL query view,
run:

```sql
SELECT id, raw_id, sysname, release, arch, num_cpus FROM machine;
```

Expect two rows. `id = 0` is always the host; remote machines have a
non-zero `raw_id`. See the [`machine` table reference][machine-table] for
the full set of columns.

To confirm that events from both machines made it into the trace, group
ftrace events by machine. `ftrace_event` does not carry `machine_id`
directly — each row references a `cpu` (via `ucpu`), and `cpu` carries
the `machine_id`:

```sql
SELECT cpu.machine_id, COUNT(*) AS num_events
FROM ftrace_event
JOIN cpu USING (ucpu)
GROUP BY cpu.machine_id;
```

You should see one row per machine, each with a non-zero count. The same
join pattern works against the `thread` or `process` tables to slice by
machine through different dimensions.

## Troubleshooting

* **Only one row in `machine`.** Connectivity problem. Check that
  `<host-ip>:20001` is reachable from `guest` (e.g. with `nc -zv`), that
  the firewall is open, and that `traced` on the host bound `0.0.0.0`
  (not `127.0.0.1`).
* **`traced_relay` exits immediately and prints usage.**
  `PERFETTO_RELAY_SOCK_NAME` is unset or empty — `traced_relay` has no
  host to forward to.
* **`traced_probes` on the guest fails with a connect error.** Make sure
  `traced_relay` is running on the guest (Step 3) and that no stale
  `traced` is also running there contesting the producer socket.
* **Producers on the host fail to connect.** Confirm `traced` started
  with `PERFETTO_PRODUCER_SOCK_NAME=0.0.0.0:20001` (Step 1) and that the
  producers are pointed at the same address (Step 2).

## Next steps

* [Multi-machine architecture](/docs/deployment/multi-machine-architecture.md) —
  the why: how `traced_relay`, machine identity, and cross-kernel clock
  sync fit together.
* [PerfettoSQL: Getting Started](/docs/analysis/perfetto-sql-getting-started.md) —
  for slicing the resulting trace by `machine_id` across `cpu`, `thread`,
  and `process`.
* [Trace Processor](/docs/analysis/trace-processor.md) — embed analysis
  in scripts or pipelines once the recording is repeatable.

[machine-table]: /docs/analysis/sql-tables.autogen#machine
