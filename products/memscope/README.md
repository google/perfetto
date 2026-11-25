# Memscope

Memscope is a memory profiler for a single app. It shows where the app's
memory goes over time, takes Java heap dumps and native heap profiles, and
puts them in one Memory Explorer page.

## In the browser

Open https://memscope.perfetto.dev, connect an Android phone over USB, and
pick the app. You can also open a trace that has heap dumps, heap profiles or
smaps snapshots.

## From the command line

```sh
products/memscope/build
out/linux_clang_release/memscope doctor --app com.example.myapp
out/linux_clang_release/memscope record --app com.example.myapp --duration 60s
```

`record` profiles native allocations for the whole run, takes Java heap dumps
with smaps spread over it (`--java-dumps`, default 2), and polls process
memory counters. When it finishes it saves the trace, prints a report and
opens it in the UI. `open <trace>` opens an existing trace.

`report <trace>` prints the numbers the Memory Explorer summary shows: peak
RSS, memory change and trend, the memory breakdown by category, and the Java
classes that grew between the first and last heap dump. It picks the process
with the most memory data, or the one given with `--app` or `--pid`.

## Shared SQL and record config

The analyses behind the summary are the `memscope` PerfettoSQL package in
`sql/memscope/`. The CLI and the UI both run it, so they report the same
numbers.

`configs/record.pbtx` is the single-app recording config used by both
`memscope record` and the UI's per-process profile.

Over SSH, the CLI prints the `ssh -L 9001:127.0.0.1:9001` command to run from
the machine with the browser. To use the UI from
`products/memscope/run-dev-server`, pass `--ui http://localhost:10000`.

The app must be profileable: on user builds it needs
`<profileable android:shell="true"/>` in its manifest. `memscope doctor`
checks this.
