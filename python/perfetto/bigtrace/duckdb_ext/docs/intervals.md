# Interval Operators

The `perfetto` DuckDB extension implements the interval
operators with both **`|>` pipe syntax**
(`src/pipe.cc`) and **`pf_interval_*` DuckDB table macros** backed by C++
aggregate kernels (`src/intervals.cc`, `sql/intervals.sql`).

## Loading

```python
import duckdb
con = duckdb.connect(config={'allow_unsigned_extensions': 'true'})
con.execute("LOAD './perfetto.duckdb_extension'")  # built by ../build.sh
```

Every interval operator can be written either as a `|>` pipe
stage (transparent in Bigtrace's DuckDB SQL mode, or via `FROM pf_pipe('...')` in
standalone DuckDB) or by calling its `pf_interval_*` table macro directly.

In `pf_interval_*` macros, relations are passed as strings: a table/view name
(`'thread_state'`) or a parenthesized SQL query (`'(SELECT * FROM slice WHERE
dur >= 0)'`). The `per` parameter (`PER (...)` in `|>` syntax) names the
partition columns, always led by `_trace_uuid` (e.g., `PER (_trace_uuid, utid)`
or `per := ['_trace_uuid', 'utid']`): DuckDB's parallel hash aggregate groups by
`_trace_uuid` and each trace is swept independently across threads, while a
trailing integer lane column (`utid`, `cpu`, `track_id`, `upid`) is bucketed and
swept inside the native kernel.

```sql
-- |> pipe syntax (in Bigtrace DuckDB SQL mode or FROM pf_pipe('...')):
INTERVAL INTERSECTION OF (
  (FROM slice |> WHERE dur >= 0) AS s,
  (FROM sched |> WHERE dur >= 0) AS c
) PER (_trace_uuid, utid)
|> AGGREGATE SUM(dur) AS on_cpu_dur, COUNT(*) AS slices
   GROUP BY _trace_uuid, s.name
|> ORDER BY _trace_uuid, on_cpu_dur DESC

-- Equivalent single-trace query (just drop _trace_uuid):
INTERVAL INTERSECTION OF (
  (FROM slice |> WHERE dur >= 0) AS s,
  (FROM sched |> WHERE dur >= 0) AS c
) PER (utid)
|> AGGREGATE SUM(dur) AS on_cpu_dur, COUNT(*) AS slices
   GROUP BY s.name
|> ORDER BY on_cpu_dur DESC
```

## Interval semantics

Identical to Trace Processor's `interval_intersect`:

- A row with `dur > 0` covers the half-open interval `[ts, ts + dur)`.
- A row with `dur = 0` is a point at `ts`. It overlaps any interval covering
  `ts` (`start <= ts < end`) and any point at the same `ts`; the overlap has
  `dur = 0`.
- Rows with `ts IS NULL` or `dur IS NULL` are ignored.
- Rows with `ts < 0` or `dur < 0` raise an error (`pf_interval: a row's ts or
  dur is below zero`). Filter unfinished slices (`dur = -1`) or close open
  intervals before calling interval operators (except `pf_interval_fill_gaps`
  and `pf_interval_find`, which have explicit `dur = -1` rules).

---

## Sources

### `pf_interval_intersection` / `pf_interval_union`

```sql
-- Lean default (full_row := false): each alias struct holds {id}, no join back
SELECT ts, dur, _trace_uuid, cpu, f.id AS freq_id, i.id AS idle_id
FROM pf_interval_intersection(
  ['cpu_freq', 'cpu_idle'],
  ['f', 'i'],
  per := ['_trace_uuid', 'cpu']
);

-- Full operand rows (full_row := true): each alias struct holds every column of that operand
SELECT ts, dur, _trace_uuid, cpu, f.freq, i.state
FROM pf_interval_intersection(
  ['cpu_freq', 'cpu_idle'],
  ['f', 'i'],
  per := ['_trace_uuid', 'cpu'],
  full_row := true
);

SELECT * FROM pf_interval_union(
  ['a', 'b'],
  ['a', 'b'],
  per := ['_trace_uuid', 'utid']
);
```

Output columns: `ts`, `dur`, the `per` columns, then one `STRUCT` column per
operand named after its alias. For `pf_interval_intersection`, `full_row :=
false` (the default) returns `{'id': ...}` per alias directly from the C++
kernel without materializing or joining payload columns; pass `full_row :=
true` to attach every column of each operand (`f.freq`, `i.state`, ...). Under
`pf_interval_union`, an operand absent from a fragment has its struct set to
`NULL` (`a.ts IS NOT NULL` tests presence).

### `pf_intervals_from_events`

```sql
-- Keep final interval open (dur = -1):
SELECT * FROM pf_intervals_from_events(
  'counter',
  per := ['_trace_uuid', 'track_id'],
  stop := 'stop_events',          -- optional relation of (per..., ts) stop points
  keep_last_open := true
);

-- Close final interval at an expression over the row:
SELECT * FROM pf_intervals_from_events(
  '(SELECT c.*, b.end_ts FROM counter c JOIN trace_bounds b USING (_trace_uuid))',
  per := ['_trace_uuid', 'track_id'],
  close_last_at := 'end_ts'
);
```

---

## Reshaping

### `pf_interval_split`

Cuts each left row at the right operand's boundaries, attaching the right row's
columns as a `STRUCT` named `alias` (`NULL` where uncovered). Lossless:
sub-intervals tile every input row.

```sql
SELECT ts, dur, _trace_uuid, utid, id, st.state
FROM pf_interval_split(
  '(SELECT * FROM slice WHERE dur >= 0 AND utid IS NOT NULL)',
  'thread_state',
  'st',
  per := ['_trace_uuid', 'utid']
);
```

### `pf_interval_subtract`

Removes the right operand's coverage; pieces outside survive with all left
payload columns intact.

```sql
SELECT * FROM pf_interval_subtract(
  '(SELECT * FROM slice WHERE dur > 0 AND utid IS NOT NULL)',
  '(SELECT * FROM thread_state WHERE state = ''Running'')',
  per := ['_trace_uuid', 'utid']
);
```

### `pf_interval_fill_gaps`

Lays the input over a non-overlapping `background` relation: input rows pass
through unchanged (a `dur = -1` input row is clipped to the end of its lane's
background), and uncovered spans of `background` become filler rows carrying the
background's columns (`NULL` for input-only columns; background-only columns are
`NULL` on input rows). If `background` has all `per` columns, lanes come from
`background` (empty input lanes are filled); otherwise every `background` row
(matched on shared `per` columns such as `_trace_uuid`) applies to every input
lane.

```sql
SELECT * FROM pf_interval_fill_gaps(
  '(SELECT * FROM slice WHERE dur >= 0)',
  '(SELECT _trace_uuid, start_ts AS ts, end_ts - start_ts AS dur, true AS filled FROM trace_bounds)',
  per := ['_trace_uuid', 'track_id']
);
```

### `pf_interval_flatten` / `pf_interval_flatten_sum`

Resolves self-overlap within each lane into disjoint segments and collapses the
active rows per segment via `aggs`. Output is `ts`, `dur`, the `per` columns
(plus `grouping_<col>` columns when `rollup := true`), and the aggregates.

Supported aggregates in `aggs`: any DuckDB aggregate expression `'AGG(...) AS
name'` (`COUNT(*)`, `SUM(x)`, `MIN(x)`, `MAX(x)`, `AVG(x)`, `ARG_MAX(val,
ord)`, `ARG_MIN(val, ord)`, `STRING_AGG(x::VARCHAR, ',')`).

`pf_interval_flatten_sum(rel, sums := [...], per := [...])` is a convenience
macro that computes `COUNT(*) AS count` and `SUM(col) AS sum_<col>` for each
column in `sums` using the zero-join C++ kernel path.

```sql
SELECT * FROM pf_interval_flatten(
  '(SELECT * FROM slice WHERE dur > 0)',
  ['COUNT(*) AS concurrency', 'ARG_MAX(name, depth) AS deepest'],
  per := ['_trace_uuid', 'track_id']
);

-- PER ROLLUP(_trace_uuid, track_id): runs at (_trace_uuid, track_id), (_trace_uuid), ()
SELECT * FROM pf_interval_flatten(
  '(SELECT * FROM slice WHERE dur > 0)',
  ['COUNT(*) AS concurrency'],
  per := ['_trace_uuid', 'track_id'],
  rollup := true
);
```

### `pf_interval_merge_overlapping` / `pf_interval_merge_consecutive`

`pf_interval_merge_overlapping` coalesces overlapping **and abutting**
(`[10, 20)` + `[20, 30)` → `[10, 30)`) intervals into coverage.
`pf_interval_merge_consecutive` coalesces only intervals that after sorting by
`ts` do not overlap and immediately abut with equal `by_cols` (`NULL = NULL`).

```sql
SELECT * FROM pf_interval_merge_overlapping(
  '(SELECT * FROM slice WHERE dur >= 0)',
  ['COUNT(*) AS n', 'SUM(dur) AS raw_dur'],
  per := ['_trace_uuid', 'track_id']
);

SELECT * FROM pf_interval_merge_consecutive(
  'thread_state',
  'state',
  ['COUNT(*) AS fragments', 'SUM(dur) AS total_dur'],
  per := ['_trace_uuid', 'utid']
);
```

### `pf_interval_set_bounds`

Replaces `ts` and `dur` from expressions over the row (`end_at` or `for_dur`).

```sql
SELECT * FROM pf_interval_set_bounds(
  'slice',
  begin_at := 'ts',
  for_dur := 'dur / 2',
  per := ['_trace_uuid']
);
```

### `pf_interval_quantize`

Cuts intervals (`dur > 0`) at fixed-width bucket boundaries `origin + k *
width` and appends `bucket` (the bucket start timestamp). `aligned_to` is
`'zero'`, `'trace_start'` (uses `bounds` table `(_trace_uuid, start_ts)` if
supplied, else `min(ts)` per partition), or any SQL expression.

```sql
SELECT _trace_uuid, cpu, bucket, SUM(dur) AS cpu_ns
FROM pf_interval_quantize(
  'sched', 100000000, 'trace_start',
  bounds := 'trace_bounds',
  per := ['_trace_uuid']
)
GROUP BY _trace_uuid, cpu, bucket;
```

---

## Matching

Relationship strings (case-insensitive):
- `'overlapping'` / `'overlapping bounds'`
- `'covering begin'` (right covers left's `ts`)
- `'covering end'` (right covers left's `ts + dur` as a point)
- `'covering bounds'` (right covers all of `[l.ts, l.ts + l.dur)`)
- `'within'` / `'within bounds'` (right lies inside `[l.ts, l.ts + l.dur)`)

### `pf_interval_join`

Attaches each matching right row as a `STRUCT` named `alias` without changing
the left row's `ts`/`dur`. `left_join := true` preserves unmatched left rows
with `alias = NULL`.

```sql
SELECT * FROM pf_interval_join(
  'frames', 'cujs', 'c', 'covering bounds',
  per := ['_trace_uuid'],
  left_join := true
);
```

### `pf_interval_find`

Attaches the single nearest right row (breaking ties by right `ts`, `dur`, row
order).
- `edge`: `'starting'` (right `ts`) or `'ending'` (right `ts + dur`; right rows
  with `dur < 0` have no end and are skipped).
- `direction`: `'after'` (`>= anchor`) or `'before'` (`<= anchor`).
- `anchor`: `'begin'`, `'end'`, `'begin of <alias>'`, `'end of <alias>'`, or a
  SQL expression over the left row (`__pf_l.<col>`).
- `within_dur`: optional maximum distance (`|edge - anchor| <= within_dur`).

```sql
WITH s1 AS (
  SELECT * FROM pf_interval_find(
    'input_events', 'frames', 'f',
    'starting', 'after', 'end',
    within_dur := 50000000,
    per := ['_trace_uuid'],
    left_join := true
  )
)
SELECT *, p.ts - ts AS latency
FROM pf_interval_find(
  's1', 'presents', 'p',
  'starting', 'after', 'end of f',
  per := ['_trace_uuid'],
  left_join := true
);
```

### `pf_interval_keep_if` / `pf_interval_drop_if`

Temporal semijoin and antijoin: each left row survives at most once. For
`'covering bounds'`, contiguous / overlapping right intervals are coalesced
first so a left interval spanned by two abutting right intervals is kept.

```sql
SELECT * FROM pf_interval_keep_if(
  '(SELECT * FROM slice WHERE dur >= 0)',
  'overlapping bounds',
  'janks',
  per := ['_trace_uuid']
);
```

---

## End-to-End Perfetto Examples

### 1. 2-way `pf_interval_intersection`: `slice` × `sched` on-CPU time per slice name

Intersect thread slices (`slice` joined with `thread_track` for `utid`) with CPU
scheduling slices (`sched`) per `['_trace_uuid', 'utid']` to compute on-CPU
running time per slice name across traces:

```sql
WITH thread_slices AS (
  SELECT
    s._trace_uuid,
    s.id,
    s.ts,
    s.dur,
    s.name,
    tt.utid
  FROM slice AS s
  JOIN thread_track AS tt
    USING (_trace_uuid, track_id)
  WHERE s.dur > 0
)
SELECT
  _trace_uuid,
  sl.name AS slice_name,
  COUNT(*) AS cpu_slices,
  SUM(dur) AS on_cpu_dur_ns
FROM pf_interval_intersection(
  ['thread_slices', '(SELECT * FROM sched WHERE dur > 0)'],
  ['sl', 'sc'],
  per := ['_trace_uuid', 'utid'],
  full_row := true
)
GROUP BY _trace_uuid, sl.name
ORDER BY on_cpu_dur_ns DESC;
```

### 2. 3-way `pf_interval_intersection`: `slice` × `thread_state` × `sched`

Intersect thread slices, thread states, and CPU scheduling intervals in a single
pass per `['_trace_uuid', 'utid']` to break down slice time by thread state and
CPU core:

```sql
WITH thread_slices AS (
  SELECT
    s._trace_uuid,
    s.id,
    s.ts,
    s.dur,
    s.name,
    tt.utid
  FROM slice AS s
  JOIN thread_track AS tt
    USING (_trace_uuid, track_id)
  WHERE s.dur > 0
)
SELECT
  _trace_uuid,
  utid,
  sl.name AS slice_name,
  st.state AS thread_state,
  sc.cpu AS cpu,
  SUM(dur) AS dur_ns
FROM pf_interval_intersection(
  [
    'thread_slices',
    '(SELECT * FROM thread_state WHERE dur > 0)',
    '(SELECT * FROM sched WHERE dur > 0)'
  ],
  ['sl', 'st', 'sc'],
  per := ['_trace_uuid', 'utid'],
  full_row := true
)
GROUP BY _trace_uuid, utid, sl.name, st.state, sc.cpu
ORDER BY dur_ns DESC;
```

### 3. Self-intersection of `slice` per track: overlapping parent/child pairs

Intersect `slice` with itself per `['_trace_uuid', 'track_id']` and filter to
strictly nested pairs (`a.depth < b.depth`) to measure how much of each parent
slice is covered by deeper slices on the same track:

```sql
SELECT
  _trace_uuid,
  track_id,
  a.id AS parent_slice_id,
  a.name AS parent_name,
  b.id AS child_slice_id,
  b.name AS child_name,
  SUM(dur) AS overlap_dur_ns
FROM pf_interval_intersection(
  [
    '(SELECT * FROM slice WHERE dur > 0)',
    '(SELECT * FROM slice WHERE dur > 0)'
  ],
  ['a', 'b'],
  per := ['_trace_uuid', 'track_id'],
  full_row := true
)
WHERE a.depth < b.depth
GROUP BY _trace_uuid, track_id, a.id, a.name, b.id, b.name
ORDER BY overlap_dur_ns DESC;
```

### 4. `pf_interval_flatten` & `pf_interval_flatten_sum` per track and per process

Flatten overlapping slices per `['_trace_uuid', 'track_id']` (to find the
deepest active slice at every instant) or sum weights across all tracks in a
process per `['_trace_uuid', 'upid']`:

```sql
-- Per-track flatten: concurrency and deepest slice name at each segment
SELECT
  _trace_uuid,
  track_id,
  ts,
  dur,
  concurrency,
  deepest_name
FROM pf_interval_flatten(
  '(SELECT * FROM slice WHERE dur > 0)',
  ['COUNT(*) AS concurrency', 'ARG_MAX(name, depth) AS deepest_name'],
  per := ['_trace_uuid', 'track_id']
);

-- Per-process flatten_sum: total active slice depth summed across a process
WITH proc_slices AS (
  SELECT
    s._trace_uuid,
    pt.upid,
    s.ts,
    s.dur,
    (s.depth + 1) AS weight
  FROM slice AS s
  JOIN process_track AS pt
    USING (_trace_uuid, track_id)
  WHERE s.dur > 0
)
SELECT
  _trace_uuid,
  upid,
  ts,
  dur,
  count AS active_slices,
  sum_weight AS total_weight
FROM pf_interval_flatten_sum(
  'proc_slices',
  ['weight'],
  per := ['_trace_uuid', 'upid']
);
```
