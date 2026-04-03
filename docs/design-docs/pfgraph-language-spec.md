# PerfettoGraph Language Specification

## Overview

PerfettoGraph (`.pfgraph.yaml`) is a YAML-based dataflow language for trace
analysis. Unlike SQL, which treats a trace as a generic relational database,
PerfettoGraph is **trace-domain-aware**: it knows that slices belong to threads
and processes, that counters are intervals, that flows link causally related
events, and that thread state breakdowns are a common analysis pattern.

The design principles:

1. **YAML format with JSON Schema validation.** The format is standard YAML,
   validated against `pfgraph-schema.json`. Any YAML tooling works.
2. **The language knows what a trace is.** Operations like `span_join`,
   `interval_intersect`, `find_ancestor`, and `flow_reachable` encode trace
   domain knowledge.
3. **Small primitive set, large standard library.** ~20 operation types.
   Complex patterns are composed from simple operations.
4. **Flat operation lists.** Each pipeline is a `table:` source followed by
   a list of transform operations. Reads top-to-bottom like a recipe.

## Format

PerfettoGraph files are YAML documents with this top-level structure:

```yaml
module: android.anrs              # Module name (dotted path)
imports: [android.process_metadata]  # Dependencies

# Function definitions
_my_helper:
  type: function
  args: {x: INT, y: STRING}
  returns: STRING
  body: |
    SELECT CASE WHEN $x > 0 THEN $y ELSE NULL END

# Pipeline definitions
_intermediate_step:               # No type = intermediate
  ops:
    - table: source_table         # First op: source
    - filter: "dur > 1000"        # Operations follow
    - select: [id, ts, dur]

public_output:                    # type: table = CREATE PERFETTO TABLE
  type: table
  ops:
    - table: _intermediate_step
    - sort: ts
```

## Pipelines

A pipeline is a named transformation defined under `ops:`:

```yaml
pipeline_name:
  type: table          # Optional: table | view | (omit for intermediate)
  index: [col1, col2]  # Optional: create index
  ops:
    - table: source    # First item: the data source
    - filter: "expr"   # Transform operations
    - select: [cols]
    - sort: col DESC
```

The first item in `ops:` is always the source (`table:`). Everything after
is a transform applied in sequence.

## Sources (`table:` — first item in ops)

| Source | Example |
|--------|---------|
| Table/view name | `table: slice` |
| Pipeline reference | `table: _my_pipeline` |
| Raw SQL | `table: {sql: "SELECT ..."}` |
| Slices with context | `table: {slices: {name: "GC*", process: "com.*"}}` |
| Interval intersection | `table: {interval_intersect: {inputs: [a, b], partition: [col]}}` |
| Union | `table: {union: [pipeline_a, pipeline_b]}` |

## Operations

### Filtering and Selection

```yaml
- filter: "dur > 1000 AND name GLOB 'binder*'"
- select: [id, ts, dur, "name AS slice_name"]
- distinct: true
- limit: 10
- offset: 20
- sort: dur DESC                    # or: sort: [dur DESC, ts ASC]
```

### Adding Columns

```yaml
# Computed columns (keeps existing + adds new)
- computed:
    end_ts: "ts + dur"
    is_long: "iif(dur > 1000000, 1, 0)"

# Columns from another table via LEFT JOIN
- add_columns:
    from: process
    on: upid = upid
    cols: [name AS process_name, pid]

# Bulk arg extraction
- extract_args:
    event_type: "event.type"
    event_seq: "event.seq"
```

### Aggregation

```yaml
- group_by:
    columns: [process_name, state]
    agg:
      total_dur: "sum(dur)"
      count: "count()"
      avg_dur: "avg(dur)"
```

### Window Functions

```yaml
- window:
    prev_state:
      expr: "lag(state)"
      partition: [utid]
      order: ts
    next_ts:
      expr: "lead(ts, 1, trace_end())"
      partition: [track_id]
      order: ts
```

### Joins

```yaml
# Chainable join (current pipeline is left side)
- join:
    right: other_table
    on: "id = other_table.id"
    type: LEFT                      # INNER (default) or LEFT

# Cross join
- cross_join: other_table
```

### Trace-Specific Operations

```yaml
# SPAN_JOIN for temporal intersection
- span_join:
    right: thread_state
    partition: [utid]
    type: LEFT                      # INNER (default) or LEFT

# Filter to intervals
- filter_during:
    intervals: startup_events
    partition: [upid]
    clip: true

# Semi-join (WHERE col IN)
- filter_in:
    match: valid_ids
    base_col: id
    match_col: id

# Set difference
- except: broken_entries

# Counter to intervals
- counter_to_intervals: true

# Classify by pattern matching
- classify:
    column: gc_type
    from: gc_name
    rules:
      "*NativeAlloc*": native_alloc
      "*young*": young
      "_": full                     # default

# Slice tree traversal
- find_ancestor:
    where: "_anc.name = 'binder reply'"
    cols: [id AS binder_reply_id, ts AS binder_reply_ts]

- find_descendant:
    where: "_desc.name = 'ScopedSetIpcHash'"
    cols: [name AS hash_name]

# Flow-based reachability
- flow_reachable:
    direction: out                  # out (default) or in

# Interval operations
- flatten_intervals: true
- merge_overlapping:
    epsilon: 0
    partition: [utid]

# Graph reachability
- graph_reachable:
    edges: _ownership_edges
    method: dfs                     # dfs (default) or bfs

# Index creation
- index: [blocking_utid, ts]
```

## Functions

```yaml
# Scalar function
_my_helper:
  type: function
  args: {x: INT, y: STRING}
  returns: STRING
  body: |
    SELECT CASE WHEN $x > 0 THEN $y ELSE NULL END

# Table-returning function
_get_slices:
  type: function
  args: {min_dur: INT}
  returns: "TABLE(id INT, ts INT, dur INT)"
  body: |
    SELECT id, ts, dur FROM slice WHERE dur > $min_dur
```

## Module System

```yaml
module: android.binder              # Declares this file's module
imports:                            # Dependencies (INCLUDE PERFETTO MODULE)
  - android.process_metadata
  - slices.with_context
```

A module is either `.sql` or `.pfgraph.yaml`. The engine auto-detects the
format at load time. Both formats can coexist — a `.pfgraph.yaml` module
can import `.sql` modules and vice versa.

## Schema Validation

The canonical schema is `docs/design-docs/pfgraph-schema.json`. It can be
used with any JSON Schema validator (e.g., Python `jsonschema`, VS Code
YAML extension) for editor support and CI validation.

## Design Rationale

### Why YAML?

- Standard format with existing tooling (editors, linters, validators)
- JSON Schema provides formal validation
- No custom parser maintenance (uses a minimal YAML subset parser)
- Familiar to developers (GitHub Actions, Docker Compose, Kubernetes)

### Why `ops:` as a flat list?

Operations have ORDER — filter before group_by produces different results
than filter after. YAML sequences preserve order. YAML mappings don't
(in spec, though most parsers do). A flat list makes the order explicit
and reads like a recipe.

### Why `table:` as the source keyword?

The first operation names the data source. `table:` is clear and
source-agnostic — it can be a table name, view name, pipeline reference,
or `{sql: "..."}` for raw SQL.

### Why keep `{sql: "..."}` as escape hatch?

Some patterns are genuinely irreducible to the dataflow model:
- `ancestor_slice()` correlated lateral joins
- Recursive CTEs for graph traversal
- `cat_stacks()`/`experimental_profile()` for pprof
- Self-joins with complex aliased column selection
- `_slice_following_flow!()` macro calls

These use `{sql: "..."}` as the source, keeping the SQL within the
pipeline system (not a standalone block).
