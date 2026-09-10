# trace_replay Design Document

## Overview

`trace_replay` is a small Linux/Android/macOS test tool that takes an existing
Perfetto trace and **replays its producer-side traffic against a real `traced`
daemon** (system or tracebox-spawned). The goal is *not* to produce a
byte-identical copy of the input trace, but to drive `traced` with realistic
write patterns so its CPU, memory, and stability can be measured.

Source: `src/tools/trace_replay/`. Binary: `out/<cfg>/trace_replay`.

The headline use case is regression testing the `TraceBufferV2` rewrite — the
tool exposes `--use-trace-buffer-v2` to flip every buffer to the new
implementation on a single command line.

## High-level flow

```
 ┌─────────────┐    ┌────────────┐    ┌──────────────────┐
 │   Analyze   │ →  │  Forge cfg │ →  │ Spawn N producer │
 │   input     │    │  + per-pid │    │  subprocesses    │
 │   .trace    │    │ replay-bin │    │  (this binary)   │
 └─────────────┘    └────────────┘    └──────────────────┘
                                              │
                                              ▼
                                       ┌──────────────┐
                                       │ traced (sys  │
                                       │ or tracebox) │
                                       └──────────────┘
                                              ▲
                                              │
                                       ┌──────────────┐
                                       │  perfetto    │
                                       │  cmdline     │
                                       │  consumer    │
                                       └──────────────┘
```

Inside, the orchestrator process runs in three phases:

1. **Analyze** the input trace, lifting out the original `TraceConfig`, the
   per-sequence buffer mapping, and a per-pid list of replayable packets.
2. **Forge** a fresh `TraceConfig` (buffers preserved, all original data
   sources replaced with `replay.bufN`) and write one binary replay file per
   producer pid.
3. **Replay** — start `traced`, fork producer subprocesses that connect via
   the SDK, then launch `perfetto -c forged.cfg.pb -o out.trace`. Once all
   producers exit, send SIGINT to perfetto and tear down.

Step 3 can be repeated `--iterations N` times back-to-back for benchmarking.

## Phase 1: trace analysis

`src/tools/trace_replay/trace_analyzer.{h,cc}`.

The input is mmap'd and walked at the `Trace.packet` level using
`protos::pbzero::Trace::Decoder`. Any `compressed_packets` payload is inflated
in-process via `trace_processor::util::GzipDecompressor::DecompressFully`
(self-contained, only depends on zlib) and the inflated bytes are recursively
re-fed into the same iterator.

For each `TracePacket` we capture:

* `trusted_pid` — the producer process this packet came from. Packets without
  `trusted_pid` are skipped.
* `trusted_packet_sequence_id` — the SDK TraceWriter id. `seq_id == 1`
  (`kServicePacketSequenceID`) is skipped because that's traced itself.
* `timestamp` and the *effective clock id* (see below).
* The raw inner bytes of the packet, **with reserved top-level fields
  stripped** (see [Reserved field stripping](#reserved-field-stripping)).
* The first `TraceConfig` packet — kept as `original_config`.
* `trace_stats.writer_stats` entries — used to recover the
  `sequence_id → buffer_index` map.

### Buffer mapping recovery

Ground truth is `trace_stats.writer_stats`, but in practice that packet is
often missing or incomplete (e.g. mid-stream snapshots, traces stopped before
final stats). The tool falls back, per sequence:

1. **By `writer_stats`** if available.
2. **By content**: inspect the payload field present in any packet on the
   sequence (`ftrace_events`, `process_stats`, `track_event`,
   `gpu_counter_event`, etc.) and map that to the corresponding data source
   name in the original `TraceConfig`, then to its `target_buffer`.
3. **Default to buffer 0** with a warning.

Strict mode (`--ignore-orphan-writers` *not* set) hard-fails if some sequences
can't be mapped at all.

### Clock handling — mini clock tracker

The tool resolves only what it can resolve cheaply: it captures the **first**
`ClockSnapshot` packet and indexes every clock id it contains, computing
`offset[clock_id] = ref_ts − this_clock_ts` against the trace's primary clock
(BOOTTIME unless overridden by
`TraceConfig.builtin_data_sources.primary_trace_clock` or
`ClockSnapshot.primary_trace_clock`).

For each packet, the **effective clock id** is resolved as:

```
per-packet timestamp_clock_id  >  TracePacketDefaults.timestamp_clock_id  >  trace default
```

Then:

* clock in offset map → translate to ref-clock ns, used for ordering and pacing.
* clock not in offset map (typically `clock_id = 64`, track_event's
  sequence-local incremental clock) → packet **fires immediately** with no
  warning. Per-sequence "last good timestamp" fallback handles ts-unset
  packets (they inherit the previous packet's translated time).

No further snapshots are consulted; suspended time / drift is ignored. This
is intentional — a tracker capable of doing more would have to pull in
`trace_processor:lib` (huge), and the tool's purpose doesn't require
sub-microsecond accuracy.

`--zero-delay` overrides everything and forces `rel_ts_ns = 0` on every
record (used for max-throughput stress tests).

### Reserved field stripping

`src/tracing/service/packet_stream_validator.cc` rejects any TracePacket
whose top-level fields include the service-injected ones: `trusted_pid`,
`trusted_packet_sequence_id`, `trusted_uid`, `trace_config`, `trace_stats`,
`synchronization_marker`, `compressed_packets`, `machine_id`, `service_event`,
`trace_provenance`, `protovms`. The original input trace has all of these
stamped onto every packet, so before persisting bytes for replay the analyzer
walks each packet with `protozero::ProtoDecoder`, drops the reserved fields,
and re-encodes the rest. Without this, every replayed packet would be
silently dropped by traced.

## Phase 2: config forging

`src/tools/trace_replay/config_forger.{h,cc}`.

`ForgeReplayConfig` builds a fresh `TraceConfig` rather than copy-then-clear
(cppgen `.gen.h` only generates `clear_<field>()` for repeated fields).

* **Kept verbatim**: `buffers` (size, fill policy). Pacing knobs that don't
  change semantics: `flush_period_ms`, `data_source_stop_timeout_ms`,
  `write_into_file`, `file_write_period_ms`, `max_file_size_bytes`,
  `compression_type`, `notify_traceur`, `allow_user_build_tracing`,
  `prefer_suspend_clock_for_duration`, `incremental_state_config`.
* **Dropped**: `data_sources`, `trigger_config`, `statsd_metadata`,
  `statsd_logging`, `android_report_config`, `enable_extra_guardrails`.
* **Replaced**: one new data source per *used* buffer index N, with
  `name = "replay.bufN"` and `target_buffer = N`.
* **`duration_ms`**: copied from `trigger_config.trigger_timeout_ms` if the
  original had a trigger config, else from `original.duration_ms`, else
  `max_rel_ts_ms + 5000` as a hard safety cap.
* **`--use-trace-buffer-v2`**: when set, every kept buffer gets
  `experimental_mode = TRACE_BUFFER_V2`.

The forged config is serialized to `<out_dir>/forged.cfg.pb`.

## Phase 3: replay file format

`src/tools/trace_replay/replay_file.{h,cc}`.

Per-pid binary file `replay-pid<PID>.bin`:

```
header  : "PREPLAY1" | uint32 num_records | uint32 num_buffers
record* : uint64 rel_ts_ns | uint32 orig_seq_id | uint32 buffer_idx |
          uint32 payload_size | bytes payload
```

Records are sorted by `rel_ts_ns` ascending. `payload` is the *inner*
TracePacket body with reserved fields removed.

## Phase 4: orchestration & producer subprocesses

`src/tools/trace_replay/orchestrator.cc` (parent) and
`src/tools/trace_replay/producer_worker.cc` (child).

### Backend selection

* **System traced** (default if reachable): probe the producer socket, and if
  `connect()` succeeds, find traced's PID by scanning `/proc/*/comm`.
* **tracebox** (`--use-tracebox`, or auto-fallback): spawn `tracebox traced`
  with `PERFETTO_PRODUCER_SOCK_NAME` and `PERFETTO_CONSUMER_SOCK_NAME` pointed
  at per-run paths under `<out_dir>/socks/`. Wait up to 5 s for the producer
  socket to appear.

The backend (specifically the tracebox subprocess, if any) is set up **once**
per `trace_replay` invocation and shared across iterations.

### Producer worker model

Each producer pid in the original trace gets its own subprocess. The parent
spawns each child by re-exec'ing itself with
`--replay-worker <replay-file> --ready-fd N` and a pipe.

Inside the worker (`producer_worker.cc`):

* `Tracing::Initialize({kSystemBackend, shmem_size_hint_kb=8192,
  shmem_page_size_hint_kb=32})`. Big SMB to absorb large ftrace bundles
  without stalling on every chunk; `kStall` exhausted policy so packets are
  never silently dropped.
* For each `buffer_idx` referenced in its replay file, register
  `ReplayDS<N>` — a template instantiation per buffer index.
  `ReplayDS<0..31>` static members are defined at the end of the .cc file
  (32 is a compile-time cap; runs that exceed it refuse to start).
* Once registered, write `'R'` to the parent's ready pipe.
* `OnStart` for each instance: capture process-wide `t0` (once via
  `std::call_once`); for every original `sequence_id` targeting this buffer,
  spawn one `std::thread`. That thread:
  1. Sleeps until `t0 + rel_ts_ns` (capped at 50 ms per sleep to stay
     responsive to OnStop).
  2. Calls `ReplayDS<N>::Trace([](auto ctx){
     ctx.NewTracePacket()->AppendRawProtoBytes(payload, size); })`. The
     SDK's TLS gives each thread its own TraceWriter, so the original
     fan-out of distinct writers is preserved.
  3. Final `ctx.Flush()` on its sequence when the records are exhausted.
* Worker exits once every thread has drained.

### Orchestration loop

```
for iter in 1..N:
    sample traced /proc/<pid>/stat            (CPU baseline)
    spawn producers, wait for 'R' on each ready pipe
    start ProcMonitor (per-iter monitor.csv, tracks peak RSS)
    optionally start `perf record -F 99 -g -p <traced_pid>` (--perf)
    spawn `perfetto -c forged.cfg.pb -o iter-K/out.trace`
    loop {
        poll producers; every 10 s emit a progress line
            (elapsed / total ms, ETA, alive count, traced RSS)
        break when all producers terminated
    }
    sample traced /proc/<pid>/stat            (CPU delta)
    SIGINT perfetto; bounded 30 s wait
    SIGINT perf if running
    stop ProcMonitor; record peak_rss_kb
SIGTERM tracebox if we spawned it
```

`base::Subprocess` from `include/perfetto/ext/base/subprocess.h` handles all
process spawning, ready-pipe `preserve_fds`, and `PR_SET_PDEATHSIG`-based
cleanup.

## Per-iteration metrics & benchmark output

For each iteration we capture:

* `wall_ms` — time from perfetto launch to last producer exit.
* `traced_cpu_user_ms` / `traced_cpu_sys_ms` — delta of utime/stime from
  `/proc/<traced_pid>/stat`, scaled by `_SC_CLK_TCK`.
* `traced_rss_peak_kb` — atomic peak observed by `ProcMonitor` during the
  iteration.
* `out_trace_bytes`.

With `--iterations N > 1`, a Google-Benchmark-style table is printed at
the end with mean / median / sample stddev / min / max for every metric.
Single-iteration runs get a compact one-shot summary.

## Output layout

```
<out_dir>/                          (default: a fresh /tmp/replay.XXXXXX)
├── forged.cfg.pb                   serialized TraceConfig
├── replay-pid<PID>.bin             one per producer pid
├── socks/                          only with --use-tracebox
│   ├── producer
│   └── consumer
└── iter-K/                         per-iteration (single-iter runs write
    ├── out.trace                    directly under <out_dir>)
    ├── monitor.csv
    └── perf.data                   only with --perf
```

## CLI

```
trace_replay [options] <input.trace>

  --out-dir DIR              Default: /tmp/replay.XXXXXX (mkdtemp)
  --iterations N             Default 1; >1 prints a benchmark table
  --use-tracebox             Spawn our own traced via tracebox
  --use-trace-buffer-v2      Set experimental_mode=TRACE_BUFFER_V2 on every
                             buffer in the forged config
  --perf                     `perf record -F 99 -g` on traced per iteration
  --monitor-interval-ms N    /proc poll interval (default 250)
  --ignore-orphan-writers    Drop packets whose seq_id can't be mapped
  --max-buffers N            Refuse traces with >N buffers (cap 32)
  --analyze-only             Stop after analysis + config forging
  --zero-delay               Force rel_ts_ns=0 on every record

Internal:
  --replay-worker FILE --ready-fd N
                             Child-process entry point. Not for human use.
```

## Known caveats

* **32-buffer compile-time cap** (`ReplayDS<0..31>`). A trace with more
  buffers is refused.
* **Real-time pacing**: a 957 s original trace takes ~957 s to replay
  (use `--zero-delay` to blast it as fast as possible).
* The output trace is *not* byte-identical to the input. `trusted_*` fields
  are re-stamped by traced, sequence ids are re-assigned by the SDK, and
  packets in clocks absent from the first ClockSnapshot fire immediately
  rather than at their original times. The traffic *shape* (per-buffer
  pressure, packet sizes, per-sequence fan-out) is what we preserve.
* The clock tracker uses only the first snapshot; if BOOTTIME and MONOTONIC
  drift later in the trace (e.g. across a suspend), replayed timestamps will
  diverge slightly from the original. Out of scope for this tool.
