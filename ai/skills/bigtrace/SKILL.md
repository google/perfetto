---
name: bigtrace
description: >-
  Open many Perfetto traces at once in the interactive Bigtrace UI, to run
  PerfettoSQL across all of them and filter, sort and compare traces by their
  metadata. Use when the user has a directory or a zip/tar archive of traces
  and wants to explore or query them together, not one at a time. Needs a
  Perfetto source checkout.
---

# Bigtrace

`tools/bigtrace_server.py` serves a directory or archive of traces to the
Bigtrace UI, which runs a PerfettoSQL query on every trace and shows the
results as one table. Run everything from the root of a Perfetto checkout.

## Start

1. Start the server in the background and keep its output. Pass the
   directory or archive (zip, tar, tar.gz, ...) the user gave you:

   ```sh
   tools/bigtrace_server.py /path/to/traces --generate-manifest
   ```

   Archives are unpacked once to `~/.cache/perfetto/bigtrace/`.
   `--generate-manifest` loads each trace once to record metadata (duration,
   device, build, process and slice counts) for filtering and sorting; later
   runs only process new or changed traces. The server is ready when it
   prints `Bigtrace server on http://127.0.0.1:5001`. If port 5001 is taken,
   add `-p <port>`.

2. If nothing is listening on port 10000, start the UI in the background:

   ```sh
   ui/run-dev-server --bigtrace
   ```

   The first run builds the UI and takes a few minutes; it is ready when
   `curl -sf http://localhost:10000/bigtrace.html` succeeds.

3. Give the user the URL the server printed:
   `http://localhost:10000/bigtrace.html?server=http://127.0.0.1:5001`.

## Sizing

By default the server keeps up to 64 traces loaded (`--pool-size`), so repeat
queries on them take well under a second, and saves a parse-once snapshot of
every trace (up to 50 GB, `--snapshot-budget-gb`) so the others reload about
4x faster than parsing. Each loaded trace takes roughly 200-500 MB; raise
`--pool-size` if memory allows. For a single scripted pass over many traces
add `--batch`, which keeps nothing.

## Stop

Stop the server with Ctrl-C or SIGTERM, not SIGKILL: it shuts down its
`trace_processor_shell` children on exit, and SIGKILL leaves them running.

All options: `tools/bigtrace_server.py --help` and
`examples/bigtrace/README.md`.
