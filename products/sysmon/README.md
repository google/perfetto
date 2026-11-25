# Sysmon

Whole-device system resource profiler built on Perfetto.

## What it is for

Sysmon answers the questions a knowledgeable, broad-based systems engineer asks
about an Android/Linux device as a whole:

- **`● Live`**: Calm, always-on (~50 KB/s, rolling 60s) real-time instrument
  cluster with `Overview` plus `Memory`, `CPU`, `Storage`, and `Thermal & Power`
  vertical sub-tabs, curated system workloads, and 1-click instant snapshots.
- **`Snapshot #N`**: Immutable 60s full-detail captures (including
  `compact_sched`, `binder`, and `block` I/O from the silent flight-recorder
  ring buffer) analyzed with progressive-disclosure explainers — zero raw
  timeline punts, multi-snapshot `Compare`, and ready-to-record handoffs to
  specialist products (`Memscope`, `Ioscope`, `lat`).
- **Agentic Right Sidebar & Shared MCP Contract**: Every view, the in-browser
  Assistant (`app.sidePanel`), and the headless MCP server share the exact same
  `sql/sysmon/*.sql` package and `tools/tools.json` manifest.

## Developing

```sh
products/sysmon/run-dev-server --port 10001
```
