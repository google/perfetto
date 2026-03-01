# Trace Processor Parse Cache

**Authors:** @lalitm

**Status:** Draft

## Problem

Loading large traces (multi-GB) into Trace Processor is slow. Parsing
protobuf trace data, sorting events, and populating tables can take minutes
for large traces. This cost is paid on every load, even when the trace
hasn't changed.

This is painful in several workflows:

1. **Interactive analysis:** Reopening the same trace in the UI after a page
   refresh or browser restart.
2. **AI agents:** Repeated open/query/close cycles on the same trace, where
   each cycle re-parses from scratch.
3. **Iterative scripting:** Python notebooks or scripts that create a new
   `TraceProcessor` instance on the same trace during development.

The parsed table representation is deterministic for a given trace + TP
version + flags. Re-parsing is pure waste when the inputs haven't changed.

## Decision

Add a parse cache that serializes parsed tables into an opaque binary
format and transparently loads from cache on subsequent opens. The caching
logic lives entirely in the shell layer — the TP core remains IO-free.

## Design

### Cache format

The parse cache format is an **internal implementation detail** of Trace
Processor. It is not stable, not versioned for external consumers, and
carries **no forwards or backwards compatibility guarantees**. The format
may change between any two TP releases without notice.

Currently, the cache is a TAR archive of Arrow IPC files (one per intrinsic
table), reusing the existing `ExportToArrow()` / Arrow TAR import
infrastructure. This is a convenient implementation choice, not a public
contract.

A separate, stable Arrow export feature (with explicit versioning and
compatibility guarantees) will be provided independently for users who need
a durable, interoperable representation. The parse cache is not that — it
is purely an acceleration mechanism tied to a specific TP build.

### Cache key

The cache is identified by:

```
SHA256(tp_version + sorted_relevant_global_flags + trace_identity)
```

Where `trace_identity` is:

| Client        | Identity                                          |
| ------------- | ------------------------------------------------- |
| CLI shell     | file path + size + mtime (from `stat()`)          |
| Browser/UI    | `File.name` + `File.size` + `File.lastModified`   |
| Python        | file path + size + mtime (from `os.stat()`)       |

This is fast to compute (no file content hashing) and sufficient for
practical invalidation. The chance of a false cache hit (different trace
content with identical name + size + mtime) is negligible.

"Relevant global flags" are those that affect parsing behavior:
`--full-sort`, `--no-ftrace-raw`, `--crop-track-events`, etc.

The inclusion of `tp_version` in the cache key means that upgrading TP
automatically invalidates all existing caches. This is intentional — schema
changes between versions make old caches unsafe to load.

### Cache location

`~/.cache/perfetto/parse-cache/<hash>` by default, overridable via
`--parse-cache-dir`. The cache directory is created on first write.

### CLI: `--parse-cache` global flag

```bash
trace_processor_shell --parse-cache query -c "SELECT ..." trace.pb
```

Behavior:

1. Compute cache key from the trace file's stat metadata + TP version +
   flags.
2. If a valid cache exists, read it and feed it to `Parse()` instead of
   the original trace. Skip to step 5.
3. Otherwise, read the original trace and feed it to `Parse()` as normal.
4. After parsing completes, start writing the parse cache in a background
   thread via `ExportToArrow()`.
5. Execute the user's command (query, repl, serve, etc.).
6. On exit, wait for the background cache write to complete before
   terminating.

The background write means the first load has no added latency for queries.
The wait-on-exit means the cache is guaranteed to exist after the first
invocation completes.

### CLI: `parse-cache` subcommand

For explicit cache management by human users:

```bash
# Create a parse cache for a trace
trace_processor_shell parse-cache create trace.pb

# Show cache status for a trace (exists, size, staleness)
trace_processor_shell parse-cache info trace.pb

# Delete cache for a specific trace
trace_processor_shell parse-cache clear trace.pb

# Delete all parse caches
trace_processor_shell parse-cache clear --all
```

### RPC: `/parse` with trace identity

To support caching for RPC clients (UI, Python), the first `/parse` call
gains an optional `trace_identity` field:

```protobuf
message ParseRequest {
  optional bytes data = 1;
  // Optional. Sent only on the first /parse call. If the shell has a
  // valid parse cache for this identity, it loads from cache and ignores
  // subsequent /parse data.
  optional string trace_identity = 2;
}

message ParseResult {
  optional string error = 1;
  // If true, the shell loaded from cache. The client may skip sending
  // further /parse chunks. Old clients that ignore this field and
  // continue sending data are fine — the shell discards the bytes.
  optional bool skip_further_parse = 2;
}
```

Flow:

1. Client sends first `/parse` chunk with `trace_identity` set.
2. Shell checks cache:
   - **Cache hit:** Load cached data, respond with
     `skip_further_parse = true`. Discard data from this and all subsequent
     `/parse` calls.
   - **Cache miss:** Parse the data normally, respond with
     `skip_further_parse = false`. Continue accepting `/parse` chunks.
3. After `notify_eof`, if cache was missed, write cache in background.
4. Old clients that don't send `trace_identity`: no caching, existing
   behavior unchanged.
5. New clients that ignore `skip_further_parse` and keep sending: shell
   discards bytes, everything still works.

This is fully backwards compatible in both directions.

### Python API

Caching is configured via `TraceProcessorConfig`:

```python
from perfetto.trace_processor import TraceProcessor, TraceProcessorConfig

config = TraceProcessorConfig(parse_cache=True)
tp = TraceProcessor(trace="large.pb", config=config)
```

Implementation: `parse_cache=True` causes the Python wrapper to pass
`--parse-cache` to the shell subprocess. The Python client also sends
`trace_identity` (from `os.stat()`) on the first `/parse` call so the
shell can check its cache. If the response has `skip_further_parse = true`,
Python skips sending remaining chunks.

No explicit cache management API in Python. Users who want manual control
can use the CLI `parse-cache` subcommand.

### What is NOT cached

- **Derived state:** Views, PerfettoSQL module outputs, and custom SQL are
  recomputed on top of the cached tables. This is expected and fast.
- **Session-specific tables:** `__intrinsic_trace_file`,
  `__intrinsic_trace_import_logs`, and similar metadata tables are excluded
  from the cache.
- **Empty tables:** Tables with zero rows are not written to the cache.
- **Stdin/pipe traces:** When there is no file identity (data piped from
  stdin or generated programmatically), caching is silently skipped.

### Disk space

The cache size is roughly comparable to the parsed table data, which can be
a significant fraction of the original trace size. Mitigations:

- Caching is always opt-in (`--parse-cache` flag or `parse-cache create`).
  Users make a conscious choice to spend disk space.
- `parse-cache info` shows cache sizes.
- `parse-cache clear --all` provides easy cleanup.
- After writing, the shell prints the cache size as a courtesy:
  `Parse cache written: 8.2 GB at ~/.cache/perfetto/parse-cache/a1b2c3`

### Concurrency

- Cache files are written atomically: write to a temporary file, then
  rename. This prevents partial reads from concurrent TP instances.
- Multiple TP instances loading the same trace simultaneously may each
  write a cache. The last rename wins, which is fine since the content is
  identical.

### Architecture summary

```
+------------------+     +-------------------+     +---------------+
| Clients          |     | Shell layer       |     | TP core       |
|                  |     | (IO lives here)   |     | (IO-free)     |
|  CLI user        |---->|                   |     |               |
|  Browser/UI      |---->| Cache check/write |---->| Parse()       |
|  Python API      |---->| File I/O          |     | ExportToArrow |
|                  |     | Background thread |     |               |
+------------------+     +-------------------+     +---------------+
```

TP core provides the primitives (`ExportToArrow`, import via `Parse`). The
shell layer orchestrates caching. RPC clients provide trace identity
metadata. No caching logic in TP core or the RPC layer itself.

## Alternatives considered

### 1. Cache in TP core

Add cache awareness to `TraceProcessor` directly (e.g., in
`NotifyEndOfFile()` or `Config`).

Pro:
* Single implementation covers all callers.

Con:
* Violates TP's IO-free design. Implicit file writes in a library are
  surprising and hard to control.
* TP doesn't know about file paths, mtime, or disk layout.

### 2. SQLite as cache format

Use the existing `ExportTraceToDatabase()` as the cache format instead.

Pro:
* Already implemented.
* Self-describing, queryable directly.

Con:
* Slower to load (SQL overhead, row-oriented).
* Larger on disk for numeric-heavy data.
* Not designed for fast bulk table restoration.

### 3. Automatic caching by default

Enable caching automatically without user opt-in.

Pro:
* Zero-friction fast reloads.

Con:
* Disk bloat can be tremendous and surprising. A 10 GB trace produces a
  comparably sized cache silently.
* Dangerous on servers, shared filesystems, CI environments.
* Users should make a conscious choice to spend disk space.

### 4. Content hashing for cache key

Use SHA256 of the full trace content as the cache key instead of
path + size + mtime.

Pro:
* Correct even if a file is modified without changing mtime.

Con:
* Hashing a multi-GB file is slow and defeats the purpose of caching.
* For RPC clients (browser), reading the full file just to compute a
  hash is wasteful.
* The mtime + size heuristic is sufficient in practice.

## Open questions

* Should there be a maximum cache directory size with automatic eviction
  (LRU)? Or is manual `parse-cache clear` sufficient?
* Should `parse-cache create` support creating caches for multiple traces
  at once (e.g., `parse-cache create *.pb`)?
* For the UI: should the cache directory be configurable via the httpd
  endpoint, or is `~/.cache/perfetto/parse-cache/` always correct?
