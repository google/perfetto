# Clock Resolution in Merged Traces

**Authors:** @LalitMaganti
**Status:** Draft
**PR:** N/A

## Problem

When the trace processor opens a "merged trace" (a zip/tar containing multiple
trace files), clock resolution is ad-hoc. Each format parser independently tries
to set the global trace clock via `SetTraceTimeClock()`. If a clock was already
set by a previously parsed file, the call is silently rejected and the trace
relies on clock snapshots from the first file.

This works in practice for the common case (proto + perf on same device) but the
behavior is implicit, undocumented, and produces surprising results when the
assumptions don't hold (e.g. two proto traces in the same zip).

A related issue is that "dumb" trace formats (JSON, etc.) that have no clock
metadata currently end up implicitly claiming `BOOTTIME` as their clock, which
is incorrect.

## Background: trace file clock capabilities

A trace file's clock capabilities are not a property of its format but of what
clock information it actually contains. There are three tiers:

1. **Full authority**: has timestamps, declares a clock domain, and provides
   `ClockSnapshot` packets mapping between clock domains. Typical of proto
   traces produced by `traced`.
2. **Semi-smart**: has timestamps and declares a clock domain, but provides no
   snapshots. Relies on another file's snapshots for resolution. Typical of
   perf traces (which declare their clock in the header) but also applies to
   synthetically generated proto traces that set a clock but omit snapshots.
3. **Dumb**: has timestamps but no clock metadata at all. Typical of JSON
   traces, but also applies to synthetically generated proto traces with no
   clock information.

Importantly, a proto trace is not inherently a full authority. A synthetically
generated proto trace can be semi-smart or entirely dumb depending on what
packets it contains.

## Design

### `TRACE_SCOPED_CLOCK` builtin clock

We introduce a new builtin clock ID: `TRACE_SCOPED_CLOCK`. This is the clock
that dumb trace formats should declare instead of incorrectly claiming a real
clock domain like `BOOTTIME`.

`TRACE_SCOPED_CLOCK` means: "this file has timestamps but does not know what
clock domain they are in." Trace processor handles disambiguating multiple files
that all declare `TRACE_SCOPED_CLOCK`; individual parsers do not need to
coordinate.

The behavior of `TRACE_SCOPED_CLOCK` depends on context:

- **Standalone trace** (e.g. opening a single JSON file): `TRACE_SCOPED_CLOCK`
  becomes the trace clock. Timestamps pass through as-is.
- **Merged trace** (e.g. JSON alongside a proto trace): trace processor adds a
  1:1 identity snapshot mapping `TRACE_SCOPED_CLOCK → <global trace clock>`.
  Timestamps pass through as-is by default. This mapping can be overridden via
  the metadata JSON (see below).

This fixes the existing bug where JSON traces implicitly get `BOOTTIME` as their
clock.

### Parse order

Files in a merged trace are parsed in a deterministic order: `traced` proto
traces first, then synthetic proto traces, then semi-smart traces (perf, etc.),
then dumb traces (JSON, etc.). This ordering ensures clock context is likely to
be established before files that depend on it are parsed.

Within the same category (e.g. among synthetic proto traces), parse order is
not guaranteed. If ordering matters, users should use the metadata JSON to
declare an explicit authority.

### Dynamic clock authority

Clock authority is **discovered during parsing**, not assumed from file format.
A trace becomes a full authority when it produces `ClockSnapshot` packets, not
because it is a proto file.

The first trace parsed (by parse order) becomes the **global clock authority**
regardless of whether it has snapshots. It:

- Sets the trace-wide clock (e.g. `BOOTTIME`, or `TRACE_SCOPED_CLOCK` if it
  declares nothing).
- Populates the **shared snapshot pool** with any `ClockSnapshot` packets it
  produces. If it produces none, the shared pool is empty.

### Per-file clock resolution rules

The core rule: **use a file's own clock snapshots if it has them, otherwise fall
back to the shared snapshot pool.**

Concretely:

- **Global clock authority (first trace):** Sets the global clock and populates
  the shared pool. May or may not have snapshots.
- **Subsequent full authority files (proto with snapshots):** Use only their own
  snapshots for clock resolution. Their snapshots do NOT enter the shared pool.
  This avoids cross-contamination: adding a second proto trace to a zip cannot
  silently shift timestamps in other files that were resolving fine against the
  first trace's snapshots.
- **Semi-smart files (perf, synthetic proto with clock but no snapshots):**
  Declare their clock domain, resolve against the shared pool.
- **Dumb files (JSON, synthetic proto with nothing):** Declare
  `TRACE_SCOPED_CLOCK`. Trace processor adds a 1:1 identity mapping to the
  global trace clock by default.

### Mid-parse snapshot discovery

A non-authority trace may start parsing without snapshots (resolving against the
shared pool) and later encounter its own `ClockSnapshot` packets. When this
happens, it switches from the shared pool to its own snapshots for all
subsequent timestamp resolution.

This transition means timestamps before the first snapshot were resolved
differently (via the shared pool) than timestamps after (via the trace's own
snapshots). This can produce a visible discontinuity. Trace processor should
flag this as an **import warning** to the user, without failing the import.

### Metadata JSON overrides

A merged trace may include a `perfetto_metadata.json` file that overrides the
default heuristics. This is the layer at which users can customize clock behavior
when the defaults don't work.

Schema:

```json
{
  "perfetto_metadata": {
    "trace_clock": {
      "id": "BOOTTIME",
      "authority": "trace_a.perfetto-trace"
    },
    "traces": {
      "inner.tar/trace_c.perf": {
        "clock_snapshot_source": "trace_b.perfetto-trace"
      }
    }
  }
}
```

Fields:

- `trace_clock.id` (optional): force a specific clock as the global trace clock,
  instead of using whichever the first trace declares.
- `trace_clock.authority` (optional): designate a specific file as the clock
  authority (i.e. whose snapshots populate the shared pool), instead of the
  first trace in parse order.
- `traces` (optional): per-file overrides, keyed by path relative to the root
  of the outermost archive. For files inside nested archives (zips inside tars
  or vice versa), use `/` as separator (e.g. `inner.tar/trace_c.perf`).
  - `clock_snapshot_source`: resolve this file's clocks using a specific other
    file's snapshots instead of the shared pool.

All fields are optional. When no metadata JSON is present, the default heuristics
apply.

## Alternatives considered

### Pool all snapshots from all authority files

Rather than restricting the shared pool to the first authority, we could merge
snapshots from all proto traces into a single pool. This would give better
temporal coverage (e.g. if the first proto trace is short but the second covers
a longer window).

Rejected because: (a) it introduces surprising behavior — adding a second proto
trace to a zip could silently change how other files' timestamps resolve;
(b) a buggy trace could corrupt clock resolution for other files; (c) clock
resolution happens during parsing, so pooling requires either a pre-pass over
all files to extract snapshots (complex) or accepting that the pool grows
mid-parse (order-dependent and confusing).

### Give every file its own scoped clock unconditionally

We could assign every file a `TRACE_SCOPED_CLOCK`, even proto and perf traces,
and rely entirely on the metadata JSON for cross-file alignment.

Rejected because: for the common case (proto + perf on same device), implicit
resolution via shared snapshots just works and requiring explicit metadata would
be a regression in usability.

## Open questions

- Naming convention and discovery mechanism for the metadata JSON within the
  zip/tar (e.g. `perfetto_metadata.json` at the root?).
- Explicit parse ordering via metadata JSON for files in the same category
  (e.g. multiple synthetic proto traces). Currently undefined; users should
  declare an explicit authority as a workaround.
- Cross-device merged traces are explicitly out of scope for now but the metadata
  JSON and `TRACE_SCOPED_CLOCK` mechanisms are designed to extend naturally to
  that case.
