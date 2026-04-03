# PerfettoGraph Language Specification

## Overview

PerfettoGraph (`.pfgraph`) is a dataflow language for trace analysis. Unlike SQL,
which treats a trace as a generic relational database, PerfettoGraph is
**trace-domain-aware**: it knows that slices belong to threads and processes,
that counters are intervals, that flows link causally related events, and that
thread state breakdowns are a common analysis pattern.

The design principles:

1. **The language knows what a trace is.** Slices have threads, processes, args,
   parents, and flow links. You don't spell out 4-table joins to get there.
2. **Raise the abstraction, don't wrap the syntax.** This is not SQL with better
   syntax. It encodes domain knowledge that eliminates entire classes of boilerplate.
3. **Small primitive set, large standard library.** ~14 runtime primitives.
   Everything else is composed via **templates** written in PerfettoGraph itself.
4. **Templates have implicit pipeline input.** Every template chains naturally
   with `.` notation, like method calls. No explicit `$source` parameter.

## Core Concepts

### Pipelines

A **pipeline** is a source followed by zero or more chained operations:

```pfgraph
result:
  table('slice')           # source
  .filter(dur > 1000)      # operation
  .select(id, ts, dur)     # operation
  .sort(dur DESC)           # operation
```

### Named Groups

Every pipeline has a name. Names are how pipelines reference each other:

```pfgraph
long_slices:
  table('slice')
  .filter(dur > 1000000)

top_10:
  long_slices              # reference to the pipeline above
  .sort(dur DESC)
  .limit(10)
```

### Dot-Chaining

Operations chain with `.`. The output of each operation is the input to the next.
This maps directly to Data Explore's visual node stack.

### Annotations

Pipelines can be annotated to control what SQL they produce:

```pfgraph
@table                     # CREATE PERFETTO TABLE
public_data:
  ...

@view                      # CREATE PERFETTO VIEW
public_view:
  ...

# No annotation = intermediate (private table if name starts with _, else view)
_private_intermediate:
  ...
```

## Primitive Operations

These are the ~14 operations that require runtime (C++ engine) support. They
cannot be decomposed into simpler PerfettoGraph operations.

### Sources (start a pipeline)

| Source | Description | Compiles to |
|--------|-------------|-------------|
| `table('name')` | Any named table or view | `FROM name` |
| `slices(name: 'glob', thread: 'glob', process: 'glob')` | Slices with context | `FROM thread_or_process_slice WHERE ...` |
| `sql('SELECT ...')` | Raw SQL escape hatch | `FROM (SELECT ...)` |
| `time_range(ts: N, dur: N)` | Single-row interval | `FROM (SELECT 0 AS id, N AS ts, N AS dur)` |
| `interval_intersect(a, b, partition: [cols])` | Temporal intersection | `FROM _interval_intersect!(...)` |
| `join(left, right, on: expr, type: INNER\|LEFT)` | Table join | `FROM left JOIN right ON ...` |
| `union(a, b, c)` | Set union | `SELECT * FROM a UNION ALL SELECT * FROM b ...` |
| `create_slices(starts: ref, ends: ref)` | Pair start/end into slices | Subquery with MIN matching |
| `lookup_table('k1' => v1, 'k2' => v2)` | Static key-value mapping | `VALUES ('k1', v1), ('k2', v2)` |

### Single-Input Operations (chain with `.`)

| Operation | Description | Compiles to |
|-----------|-------------|-------------|
| `.filter(expr)` | Row selection | `WHERE expr` |
| `.select(col1, col2 AS alias)` | Column projection (replaces columns) | `SELECT col1, col2 AS alias` |
| `.computed(name: expr, ...)` | Add derived columns (keeps existing) | `SELECT *, expr AS name` |
| `.group_by(cols).agg(name: func(col))` | Aggregation | `GROUP BY cols` + aggregate SELECT |
| `.window(name: func(col) over (...))` | Window functions | `SELECT *, func() OVER (...) AS name` |
| `.sort(col DESC)` | Ordering | `ORDER BY col DESC` |
| `.limit(N)` | Row limit | `LIMIT N` |
| `.offset(N)` | Row offset | `OFFSET N` |
| `.counter_to_intervals()` | Counter points → intervals | `counter_leading_intervals!(...)` |
| `.span_join(right, partition: [cols])` | Temporal join | `SPAN_JOIN` virtual table |
| `.closest_preceding(other, match: col, order: ts)` | Temporal as-of join | LEFT JOIN + row_number + filter |
| `.filter_during(intervals, partition: [cols])` | Keep rows within intervals | `_interval_intersect!(...)` |
| `.filter_in(ref, base_col: col, match_col: col)` | Semi-join | `WHERE col IN (SELECT ...)` |
| `.except(ref)` | Set difference | `EXCEPT SELECT * FROM ref` |
| `.distinct()` | Remove duplicates | `SELECT DISTINCT` |
| `.index(col1, col2)` | Create index (on @table) | `CREATE PERFETTO INDEX` |

## Parser Sugar

These operations desugar at parse time into combinations of primitives. They
exist because certain patterns are so common that dedicated syntax dramatically
improves readability.

### `.classify(result_col, from: source_col, 'pattern' => 'value', ...)`

Desugars to `.computed()` with a CASE/WHEN expression.

```pfgraph
# Sugar:
.classify(gc_type, from: name,
  '*NativeAlloc*' => 'native_alloc',
  '*young*' => 'young',
  _ => 'full'
)

# Desugars to:
.computed(gc_type: CASE
  WHEN name GLOB '*NativeAlloc*' THEN 'native_alloc'
  WHEN name GLOB '*young*' THEN 'young'
  ELSE 'full'
END)
```

Rules: patterns containing `*` or `?` use `GLOB`. Exact strings use `=`.
`_` is the default (`ELSE`).

### `.extract_args(name: 'arg.path', ...)`

Desugars to `.computed()` with `extract_arg()` calls.

```pfgraph
# Sugar:
.extract_args(
  event_type: 'event.type',
  event_seq: 'event.seq'
)

# Desugars to:
.computed(
  event_type: extract_arg(arg_set_id, 'event.type'),
  event_seq: extract_arg(arg_set_id, 'event.seq')
)
```

### `.parse_name(template)`

Desugars to `.computed()` with `str_split()`/`substr()`/`instr()` calls. The
template uses `{field_name}` as capture placeholders.

```pfgraph
# Sugar:
.parse_name('ErrorId:{process_name} {pid}#{error_id}')

# Desugars to:
.computed(
  process_name: str_split(substr(str_split(name, '#', 0), 9), ' ', 0),
  pid: str_split(substr(str_split(name, '#', 0), 9), ' ', 1),
  error_id: str_split(name, '#', 1)
)
```

The compiler analyzes the template string to determine the appropriate
`str_split`/`substr` chain for each field.

### `lookup_table('key' => value, ...)`

Desugars to a `sql()` source with a VALUES expression.

```pfgraph
# Sugar:
anr_durations:
  lookup_table(
    'BROADCAST_OF_INTENT' => 60000,
    'INPUT_DISPATCHING_TIMEOUT' => 5000,
    'EXECUTING_SERVICE' => 20000
  )

# Desugars to:
anr_durations:
  sql('SELECT key, value FROM (VALUES
    (''BROADCAST_OF_INTENT'', 60000),
    (''INPUT_DISPATCHING_TIMEOUT'', 5000),
    (''EXECUTING_SERVICE'', 20000)
  )')
```

Produces columns `key` (STRING) and `value`.

## Templates (`@define`)

Templates are the composition mechanism. They allow defining reusable pipeline
patterns **in PerfettoGraph itself**, rather than baking them into the runtime.

### Operation Templates

An operation template transforms an incoming pipeline. Its body starts with `.`
(dot), meaning "continue from the implicit input":

```pfgraph
@define thread_state_breakdown_pivoted(utid_col: Column):
  .span_join(table('thread_state'), partition: [$utid_col])
  .group_by($utid_col)
  .agg(
    running_dur: sum(iif(state = 'Running', dur, 0)),
    runnable_dur: sum(iif(state IN ('R', 'R+'), dur, 0)),
    sleeping_dur: sum(iif(state = 'S', dur, 0)),
    uninterruptible_io_dur: sum(iif(state = 'D' AND io_wait = 1, dur, 0)),
    uninterruptible_dur: sum(iif(state = 'D' AND io_wait != 1, dur, 0))
  )
```

Usage (reads like English — "binder transactions, broken down by thread state"):

```pfgraph
binder_state:
  binder_txns
  .thread_state_breakdown_pivoted(utid_col: client_utid)
```

### Source Templates

A source template produces a pipeline from nothing. Its body starts with a
source (`table()`, `join()`, etc.), not a `.`:

```pfgraph
@define process_counter_intervals(track_name: String):
  join(table('counter') AS c, table('process_counter_track') AS pct,
       on: c.track_id = pct.id, type: INNER)
  .filter(pct.name = $track_name)
  .counter_to_intervals()
```

Usage:

```pfgraph
heap_size:
  process_counter_intervals('Heap size (KB)')
  .filter(process_name GLOB 'com.google.*')
```

### The Distinction

The parser determines the template type by looking at the first token of the body:
- Starts with `.` → **operation template** (requires implicit input, chains with `.`)
- Starts with a source → **source template** (starts a new pipeline)

No annotation needed — it's inferred.

### Parameter Types

| Type | Description | Example |
|------|-------------|---------|
| `Column` | A column name, substituted as identifier | `utid_col: Column` |
| `String` | A string literal | `track_name: String` |
| `Int` | An integer literal | `threshold: Int` |
| `Pipeline` | A pipeline reference (for secondary inputs) | `intervals: Pipeline` |

### Importing Templates

Templates live in `.pfgraph` files and are imported by module path:

```pfgraph
import std.thread_state    # thread_state_breakdown, etc.
import std.counter         # process_counter_intervals, etc.
import std.flow            # flow_targets, flow_sources
```

## Standard Library

The standard library is written in PerfettoGraph and ships with trace_processor.
Users can inspect and modify any template.

### `std.thread_state`

```pfgraph
# Thread state breakdown: intersect with thread_state, aggregate by state.
@define thread_state_breakdown(utid_col: Column):
  .span_join(table('thread_state'), partition: [$utid_col])
  .group_by($utid_col, state, io_wait)
  .agg(total_dur: sum(dur), count: count())

# Pivoted version: one column per state (Running, Runnable, Sleeping, etc.)
@define thread_state_breakdown_pivoted(utid_col: Column):
  .span_join(table('thread_state'), partition: [$utid_col])
  .group_by($utid_col)
  .agg(
    running_dur: sum(iif(state = 'Running', dur, 0)),
    runnable_dur: sum(iif(state IN ('R', 'R+'), dur, 0)),
    sleeping_dur: sum(iif(state = 'S', dur, 0)),
    uninterruptible_io_dur: sum(iif(state = 'D' AND io_wait = 1, dur, 0)),
    uninterruptible_dur: sum(iif(state = 'D' AND io_wait != 1, dur, 0))
  )
```

### `std.counter`

```pfgraph
# Process-scoped counter as intervals (ts, dur, value, upid, process_name).
@define process_counter_intervals(track_name: String):
  join(table('counter') AS c, table('process_counter_track') AS pct,
       on: c.track_id = pct.id, type: INNER)
  .filter(pct.name = $track_name)
  .counter_to_intervals()

# Add a counter value at a given timestamp to each row.
@define with_counter_value(track_name: String, by: Column, as: Column):
  .closest_preceding(
    process_counter_intervals($track_name),
    match: $by = upid,
    order: ts
  )
  .computed($as: value)
```

### `std.flow`

```pfgraph
# Follow flow links outward: adds target slice columns.
@define flow_targets():
  .add_columns(
    from: join(table('flow') AS f, table('slice') AS tgt,
               on: f.slice_in = tgt.id, type: INNER),
    on: id = f.slice_out,
    cols: [tgt.id AS target_id, tgt.ts AS target_ts,
           tgt.dur AS target_dur, tgt.name AS target_name,
           tgt.track_id AS target_track_id]
  )

# Follow flow links inward: adds source slice columns.
@define flow_sources():
  .add_columns(
    from: join(table('flow') AS f, table('slice') AS src,
               on: f.slice_out = src.id, type: INNER),
    on: id = f.slice_in,
    cols: [src.id AS source_id, src.ts AS source_ts,
           src.dur AS source_dur, src.name AS source_name]
  )
```

### `std.sequence`

```pfgraph
# Add next row's columns (for sequence navigation).
@define with_next(partition_col: Column, order_col: Column):
  .window(
    next_ts: lead(ts) over (partition: [$partition_col], order: $order_col),
    next_dur: lead(dur) over (partition: [$partition_col], order: $order_col),
    next_id: lead(id) over (partition: [$partition_col], order: $order_col)
  )

# Add previous row's columns.
@define with_prev(partition_col: Column, order_col: Column):
  .window(
    prev_ts: lag(ts) over (partition: [$partition_col], order: $order_col),
    prev_dur: lag(dur) over (partition: [$partition_col], order: $order_col),
    prev_id: lag(id) over (partition: [$partition_col], order: $order_col)
  )
```

## Declarations

### Module and Imports

```pfgraph
module android.binder      # declares this file's module path
import android.process_metadata   # INCLUDE PERFETTO MODULE
import std.thread_state           # import stdlib templates
```

### Functions

Scalar functions:

```pfgraph
@function _double(x: INT) -> INT:
  sql('SELECT $x * 2')
```

Table-returning functions:

```pfgraph
@function _get_slices(min_dur: INT) -> TABLE(id: INT, ts: INT, dur: INT):
  table('slice')
  .filter(dur > $min_dur)
  .select(id, ts, dur)
```

### SQL Escape Hatch

For truly irreducible patterns (recursive CTEs, `cat_stacks`/`experimental_profile`,
`CREATE PERFETTO MACRO`):

```pfgraph
@sql {
  CREATE PERFETTO MACRO _my_macro(t TableOrSubquery)
  RETURNS TableOrSubquery AS
  SELECT * FROM $t WHERE dur > 0;
}
```

The goal is to rarely need this.

## Module System Interop

PerfettoGraph uses the same module system as PerfettoSQL. Understanding the
interop is critical because the stdlib will be a mix of `.sql` and `.pfgraph`
files during migration.

### Module identity

A module's identity is its **include key**, derived from the file path by
stripping the extension and replacing `/` with `.`:

```
android/binder.sql     → android.binder
android/binder.pfgraph → android.binder  (SAME key!)
```

This means a module is EITHER `.sql` OR `.pfgraph`, never both. Migration
from `.sql` to `.pfgraph` is a file replacement.

### How `import` works

```pfgraph
import android.process_metadata   # Loads android/process_metadata.{sql,pfgraph}
import std.thread_state           # Loads std/thread_state.pfgraph (stdlib template)
```

The `import` statement compiles to `INCLUDE PERFETTO MODULE android.process_metadata;`.
When trace_processor encounters this:
1. It looks up the include key in its registered packages
2. If the module is a `.pfgraph` file, it's compiled to SQL first, then executed
3. If the module is a `.sql` file, it's executed directly
4. The `included` flag prevents re-inclusion (handles transitive deps)

### Cross-format dependencies

A `.pfgraph` module can depend on `.sql` modules and vice versa. The compiled
output of a `.pfgraph` file is SQL that creates the same tables/views/functions
as a hand-written `.sql` file would. From the consumer's perspective, the
format is invisible:

```pfgraph
# This .pfgraph file imports a .sql module — works fine.
import android.process_metadata   # This is a .sql file

@table
enriched_slices:
  slices(name: 'binder*')
  .add_columns(from: table('android_process_metadata'),
               on: upid = upid, cols: [package_name])
```

```sql
-- This .sql file imports a .pfgraph module — also works fine.
INCLUDE PERFETTO MODULE android.anrs;  -- This could be a .pfgraph file

SELECT * FROM android_anrs WHERE anr_type = 'INPUT_DISPATCHING_TIMEOUT';
```

### Migration strategy

Modules can be migrated one at a time, in any order:

1. Replace `foo/bar.sql` with `foo/bar.pfgraph`
2. The compiled SQL produces the same tables/views/functions
3. All consumers (both `.sql` and `.pfgraph`) continue to work unchanged
4. No coordinated migration needed

### Template modules

Templates defined with `@define` in a `.pfgraph` file are **compile-time only**.
They don't produce SQL output — they're expanded at the call site. This means:

```pfgraph
# std/thread_state.pfgraph
module std.thread_state

@define thread_state_breakdown(utid_col: Column):
  .span_join(table('thread_state'), partition: [$utid_col])
  .group_by($utid_col, state, io_wait)
  .agg(total_dur: sum(dur), count: count())
```

When another module does `import std.thread_state`, the template definitions
are loaded into the compiler's template registry. No SQL is executed — the
templates only produce SQL when called.

A `.pfgraph` file can mix templates and concrete pipelines:

```pfgraph
module android.binder
import std.thread_state    # templates only — no SQL side effects
import std.flow            # templates only
import android.process_metadata  # concrete SQL: INCLUDE PERFETTO MODULE

# This template is local to this module:
@define _with_aidl_name():
  .add_columns(
    from: table('slice'),
    on: target_id = parent_id,
    cols: [name AS aidl_name]
  )
  .filter(aidl_name GLOB 'AIDL::*')

# This creates a real table (produces SQL):
@table
android_binder_txns:
  slices(name: 'binder transaction')
  .flow_targets()           # from std.flow
  ._with_aidl_name()        # local template
  .thread_state_breakdown_pivoted(utid_col: utid)  # from std.thread_state
```

### Public vs private names

Same convention as PerfettoSQL: names starting with `_` are private. This applies
to pipelines, templates, and functions:

```pfgraph
# Private (internal implementation detail):
_raw_data:
  table('slice').filter(dur > 0)

@define _helper_template():
  .filter(name IS NOT NULL)

@function _internal_func(x: INT) -> INT:
  sql('SELECT $x * 2')

# Public (part of module API):
@table
android_anrs:
  _raw_data._helper_template()
```

## Grammar (EBNF)

```ebnf
program       = [module_decl] {import_decl} {declaration}

module_decl   = 'module' dotted_name
import_decl   = 'import' dotted_name

declaration   = annotation named_pipeline
              | '@sql' '{' raw_sql '}'
              | '@function' function_decl
              | '@define' template_decl

annotation    = ['@table' | '@view']

named_pipeline = IDENT ':' pipeline

pipeline      = source {'.' operation}

source        = 'table' '(' STRING ')'
              | 'slices' '(' kv_args ')'
              | 'sql' '(' STRING ')'
              | 'time_range' '(' kv_args ')'
              | 'interval_intersect' '(' ref_list [',' 'partition' ':' list] ')'
              | 'join' '(' ref ',' ref ',' kv_args ')'
              | 'union' '(' ref_list ')'
              | 'create_slices' '(' kv_args ')'
              | 'lookup_table' '(' mapping_list ')'
              | IDENT                                    (* pipeline reference *)
              | IDENT '(' arg_list ')'                   (* template call *)

operation     = 'filter' '(' expr ')'
              | 'select' '(' column_list ')'
              | 'computed' '(' named_expr_list ')'
              | 'group_by' '(' ident_list ')' '.' 'agg' '(' agg_list ')'
              | 'window' '(' window_spec_list ')'
              | 'sort' '(' sort_list ')'
              | 'limit' '(' INT ')'
              | 'offset' '(' INT ')'
              | 'add_columns' '(' kv_args ')'
              | 'counter_to_intervals' '(' ')'
              | 'span_join' '(' ref [',' kv_args] ')'
              | 'closest_preceding' '(' ref ',' kv_args ')'
              | 'filter_during' '(' ref [',' kv_args] ')'
              | 'filter_in' '(' ref ',' kv_args ')'
              | 'except' '(' ref ')'
              | 'distinct' '(' ')'
              | 'index' '(' ident_list ')'
              | 'classify' '(' IDENT ',' 'from' ':' IDENT ',' mapping_list ')'
              | 'extract_args' '(' named_string_list ')'
              | 'parse_name' '(' STRING ')'
              | IDENT '(' [arg_list] ')'                 (* template call *)

template_decl = IDENT '(' param_list ')' ':' pipeline_body
function_decl = IDENT '(' param_list ')' '->' return_type ':' body

pipeline_body = '.' operation {'.' operation}            (* operation template *)
              | source {'.' operation}                   (* source template *)

param_list    = param {',' param}
param         = IDENT ':' TYPE

return_type   = TYPE | 'TABLE' '(' column_type_list ')'
body          = pipeline | 'sql' '(' STRING ')'

mapping_list  = mapping {',' mapping}
mapping       = (STRING | '_') '=>' (STRING | INT | FLOAT)

window_spec   = IDENT ':' expr 'over' '(' window_clause ')'
window_clause = ['partition' ':' list] ['order' ':' IDENT ['DESC'|'ASC']]
                ['frame' ':' STRING]

expr          = (* balanced expression, passed through as SQL *)
```

## Design Rationale

### Why implicit pipeline input?

Because it makes templates compose naturally with dot-chaining:

```pfgraph
# Without implicit input (awkward):
binder_state:
  thread_state_breakdown_pivoted(binder_txns, utid_col: client_utid)

# With implicit input (natural):
binder_state:
  binder_txns
  .thread_state_breakdown_pivoted(utid_col: client_utid)
```

The second form reads like Data Explore looks: a stack of nodes, top to bottom.

### Why templates instead of more builtins?

Because builtins are opaque. If `thread_state_breakdown` is a builtin, you can't:
- See how it works
- Modify it for your use case
- Build your own similar patterns

As a template, it's 5 lines of PerfettoGraph that anyone can read, copy, modify.
Like how `qsort` is written in C, not baked into the compiler.

### Why `.parse_name()` as sugar instead of a primitive?

Because it desugars to `str_split`/`substr` calls that are already in SQL. The
sugar exists because the pattern is incredibly common (30+ stdlib files parse
structured strings from names) and the desugared form is unreadable.

### Why keep `@sql {}` at all?

Some things are genuinely irreducible to the dataflow model:
- `CREATE PERFETTO MACRO` (parameterized by table types)
- Recursive CTEs (graph algorithms)
- `cat_stacks`/`experimental_profile` (pprof generation)

These are the "inline assembly" of PerfettoGraph. The goal is <5% of code.
