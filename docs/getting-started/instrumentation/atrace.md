# Instrumenting Android apps/platform with atrace

_In this page, you'll TODO_

## Introduction

- atrace is Android's tracing system
- Been around since SDK X
- Explicit annotations added to code to give context on what the app is doing
- Lot of instrumentation already in the platform for APIs that apps call
- system processes (e.g. system_server) is also extensively instrumented so even
  without any additional instrumentation, can get a lot of insight into what
  your process is doing.
- NOTE: does not work for high performance but for most "app-level" stuff. Rough
  guide: if function is called less often than once every 10ms, tracing is fine,
  more it can start to have a perf impact.

## Adding atrace instrumentation

<?tabs>

TAB: Java

Use record_android_trace script

TAB: C++

Go through example on adding atrace

</tabs?>

## Viewing your recorded trace

Video: open trace in Perfetto UI, navigate around

## Instrumentation types (quick reference)

### App and platform developers

Slices: operations on a single thread

Async slices: operations spanning multiple threads

Counters

### Platform developers only

Instants

Async slices on track

## Next Steps

Collect system traces: see other page

Non-atrace instrumentation
