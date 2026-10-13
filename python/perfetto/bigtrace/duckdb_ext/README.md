# perfetto-duckdb

A DuckDB C-API extension (`perfetto.duckdb_extension`) implementing **PerfettoSQL Next** ([`docs/spec.md`](docs/spec.md): §§2–4 `|>` pipeline syntax, §5 interval operators, §6 tree operators, §7 graph operators) over tables that hold many traces.

Raw trace tables (`slice`, `sched`, `thread_state`, `counter`, `heap_graph_*`, ...) are extracted once from trace processor into DuckDB (or Parquet) with a trace column (`_trace_uuid` in the bigtrace server, an ENUM), and every operator then runs over thousands of traces in one query, trace by trace, in parallel.

## Quick start

Requires a C++17 compiler and Python 3; the DuckDB C extension headers (v1.5.6) are vendored in [`src/`](src/).

```bash
./build.sh   # -> perfetto.duckdb_extension
```

```python
import duckdb
con = duckdb.connect(config={'allow_unsigned_extensions': 'true'})
con.execute("LOAD './perfetto.duckdb_extension'")

# 1. PerfettoSQL Next |> pipeline syntax (via pf_pipe(...) or pf_compile_pipe(...)):
con.sql("""
  FROM pf_pipe('
    INTERVAL INTERSECTION OF (thread_state AS ts, sched AS s) PER (_trace_uuid, utid)
    |> AGGREGATE SUM(dur) AS dur GROUP BY _trace_uuid, ts.state, s.cpu
  ')
""")

# 2. Direct pf_* table macro syntax:
con.sql("SELECT * FROM pf_tree_accumulate_up('slice', 'dur', per := ['_trace_uuid'])")
con.sql("""SELECT * FROM pf_interval_intersection(['thread_state', 'sched'], ['ts', 's'],
                                                 per := ['_trace_uuid', 'utid'])""")
```

## PerfettoSQL Next (`|>`) Pipeline Syntax & Copy-Paste Equivalence

You can write queries using either the **PerfettoSQL Next `|>` chain syntax** (`docs/spec.md` §§2–7) or the underlying `pf_*` table macros:

- **In Bigtrace UI / Python Server (`engine="duckdb"`)**: `DuckDBEngine` automatically runs `pf_compile_pipe(sql)` before executing, so you can write `FROM ... |> ...`, `INTERVAL INTERSECTION OF ... |> ...`, `GRAPH DOMINATOR TREE ... |> ...`, `FORK AS ...`, and `CREATE [OR REPLACE] PERFETTO TABLE/VIEW ... AS ...` directly in the DuckDB SQL editor without wrapping in `pf_pipe(...)`.
- **In standalone DuckDB Python/CLI**: wrap a pipeline in `FROM pf_pipe('...')`, or compile a multi-statement script with `SELECT pf_compile_pipe('...')`.
- **Moving a query between Multi-Trace DuckDB and Single-Trace PerfettoSQL**:
  - The `|>` pipeline syntax is identical on both sides (`WHERE`, `SELECT`, `SELECT * EXCEPT (...)`, `EXTEND`, `SET`, `DROP`, `RENAME`, `DISTINCT`, `AGGREGATE ... GROUP BY [ROLLUP(...)]`, `ORDER BY`, `LIMIT`, `JOIN`, `UNION/INTERSECT/EXCEPT`, `WINDOW`, `SQL(... @input ...)`, `FORK AS`, duration literals like `16ms`/`500us`/`1.5s`, and all `INTERVAL`, `TREE`, and `GRAPH` stages).
  - To run a multi-trace DuckDB `|>` query on a single trace in PerfettoSQL, simply drop `_trace_uuid` from `PER (_trace_uuid, ...)` and `GROUP BY _trace_uuid, ...` (or drop `PER (_trace_uuid)` entirely when `_trace_uuid` was the only partition key).

## The per-trace contract

Every operator takes `per := [...]`, the partition columns. **Lead with the
trace column** (`_trace_uuid`, any type: ENUM, VARCHAR or integer, never cast);
rows of different partitions never interact.

* The leading `per` columns are the `GROUP BY` of the operator's native kernel,
  an aggregate function: DuckDB routes each trace's rows to one state and
  finalizes states in parallel (one trace per task, no cross-thread merge,
  per-trace working sets that fit in cache).
* For interval operators the **last** `per` column (with at least two) is the
  *lane* (`utid`, `track_id`, `cpu`, `upid`: an integer column). Lanes are
  handled inside the kernel: renumbered densely per trace, bucketed with a
  stable counting sort and swept one by one.
* Kernels exploit TP's physical order: tables are produced trace by trace,
  `ts`-ordered (intervals) and with ids `0..n-1`, parents before children
  (slices). Sortedness is checked in O(n); sorted runs from parallel scans are
  merged; only genuinely unordered input is sorted. Slice trees with dense ids
  are solved with array indexing only (no hashing, no sort).
* Relations are table / view names or parenthesized queries (`'(SELECT ...)'`).
* Operators that keep rows (`pf_tree_accumulate`, `pf_tree_keep_if`,
  `pf_tree_drop_if`, `pf_tree_prune`, graph trees) take `full_row`: `true`
  (default) returns every input column (a join back to the input), `false`
  returns just `per`, `id` (+ `parent_id` / results) straight from the kernel,
  which is several times faster on large inputs. `pf_tree_accumulate_up/down`
  are always lean. Join payload yourself when you need only a few columns:
  `FROM pf_tree_accumulate_up('slice', 'dur', per := ['_trace_uuid']) a JOIN slice USING (_trace_uuid, id)`.

## Operators

All operators are `pf_*` table macros. The macro calls a C++ SQL generator
(`pf__sql_*`, [`src/sql_gen.cc`](src/sql_gen.cc)) at bind time (about 1 ms)
and runs the generated kernel pipeline with `query()`.

- **Intervals ([`docs/intervals.md`](docs/intervals.md))**:
  - Sources: `pf_interval_intersection`, `pf_interval_union`, `pf_intervals_from_events`
  - Reshaping: `pf_interval_flatten`, `pf_interval_flatten_sum`, `pf_interval_split`, `pf_interval_subtract`, `pf_interval_fill_gaps` (`pf_interval_fill_within`), `pf_interval_merge_overlapping`, `pf_interval_merge_consecutive`, `pf_interval_set_bounds`, `pf_interval_quantize`
  - Matching: `pf_interval_join`, `pf_interval_find`, `pf_interval_keep_if`, `pf_interval_drop_if`
- **Trees ([`docs/trees.md`](docs/trees.md))**:
  - Accumulation: `pf_tree_accumulate`, `pf_tree_accumulate_up`, `pf_tree_accumulate_down`
  - Filtering & reshaping: `pf_tree_keep_if`, `pf_tree_drop_if`, `pf_tree_prune`, `pf_tree_reparent`, `pf_tree_flatten_recursion`, `pf_tree_contract`, `pf_tree_absorb`
  - Callstacks / flamegraphs: `pf_tree_key_siblings`, `pf_tree_merge_siblings`, `pf_tree_merge_into_parent`, `pf_tree_invert`, `pf_tree_expand`
- **Graphs ([`docs/graphs.md`](docs/graphs.md))**: `pf_graph_dfs_tree`, `pf_graph_bfs_tree`, `pf_graph_dominator_tree`

## Performance

100 traces (19.6M slices), same inputs on both sides, checksums match per
trace. PSN: sum over traces of `CREATE PERFETTO TABLE r AS <op>` CPU time.
DuckDB: one query over all traces; 1T = CPU time, 16T / all-T = wall time;
consumer reads every output column (`count(*), max(..)`), no materialization.

| op | PSN CPU (s) | DuckDB 1T (s) | 16T (s) | all-T (s) |
|---|---|---|---|---|
| tree_up | 0.646 | 0.27–0.32 | 0.108 | 0.064 |
| tree_down | 0.579 | 0.28–0.43 | 0.098 | 0.067 |
| flatten_count | 0.839 | 0.32–0.33 | 0.150 | 0.129 |
| flatten (3 aggs) | 1.421 | 0.53–0.56 | 0.208 | 0.303 |
| intersect depth0 x depth1 | 0.659 | 0.51 | 0.098 | 0.078 |
| intersect thread_state x sched | 1.376 | 0.61–0.69 | 0.208 | 0.190 |
| intersect 3-way | 0.716 | 0.54–0.58 | 0.130 | 0.109 |
| self-intersect slices | 6.943 | 2.5–2.8 | 0.903 | – |
| keep_if descendant | 3.740 | 0.26 | 0.065 | 0.046 |
| keep_if ancestor | 0.312 | 0.26 | 0.064 | 0.049 |
| graph dfs / bfs / dominator (22 heap dumps) | 35.5 / 36.9 / 83.1 | 14.5 / 13.3 / 25.7 | 8.2 / 4.2 / 8.8 | 6.3 / 2.9 / 6.9 |

Per row, single-threaded kernels are roughly on par with PSN (0.5x–1.7x on a
single trace, see `bench/fairness.py`); writing the output into a DuckDB table
(`CREATE TABLE AS`) costs DuckDB extra, making tree ops ~0.6x PSN. The gains come
from running one query over many traces in parallel.

Reproduce with [`bench/bench_ops.py`](bench/bench_ops.py) (intervals, trees),
[`bench/bench_graph_ops.py`](bench/bench_graph_ops.py) (graphs), and
[`bench/fairness.py`](bench/fairness.py) (single-trace fairness checks).

## Testing

```bash
python3 tests/test_partition_kernels.py  # per-trace contract: key types, order, threads, edge cases
python3 tests/test_intervals.py          # brute-force references + TP oracle on real traces
python3 tests/test_trees.py
python3 tests/test_graphs.py             # incl. TP oracle on Java heap dumps
```

## Layout

- [`src/common.h`](src/common.h) — partition-kernel building blocks: block row store (+ recycling pool), aggregate state plumbing, LIST<STRUCT> output writer, dense key renumbering
- [`src/pipe.cc`](src/pipe.cc) — PerfettoSQL Next (`|>`) pipeline parser and compiler (`pf_compile_pipe`, `pf_pipe`) covering `docs/spec.md` §§2–7
- [`src/sql_gen.cc`](src/sql_gen.cc) — SQL generation behind the macros
- [`src/intervals.cc`](src/intervals.cc), [`sql/intervals.sql`](sql/intervals.sql), [`docs/intervals.md`](docs/intervals.md) — interval operators (§5)
- [`src/trees.cc`](src/trees.cc), [`sql/trees.sql`](sql/trees.sql), [`docs/trees.md`](docs/trees.md) — tree operators (§6)
- [`src/graphs.cc`](src/graphs.cc), [`sql/graphs.sql`](sql/graphs.sql), [`docs/graphs.md`](docs/graphs.md) — graph operators (§7)
- [`src/proto_ingest.cc`](src/proto_ingest.cc) — C++ HTTP `/query` client, chunked `QueryResult` protobuf decoder, bounded MPMC queue, and `pf_scan_tp_stream` table function for zero-Python trace ingestion
- [`src/extension.cc`](src/extension.cc), [`package_extension.py`](package_extension.py), [`build.sh`](build.sh) — entry point and packaging
- [`tests/`](tests/), [`bench/`](bench/)
