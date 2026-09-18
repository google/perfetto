# Pipeline API benchmarks

Compare equivalent queries through four native paths:

| Backend | Query and storage |
| --- | --- |
| `pipeline` | PipeSQL over a finalized Perfetto dataframe |
| `sqlite` | Ordinary SQL over SQLite tables in the same Perfetto build |
| `dataframe` | Ordinary SQL over finalized Perfetto dataframes via SQLite |
| `duckdb` | Ordinary SQL over DuckDB tables through its native C API |
| `pipeline_sql_scan` (optional) | PipeSQL with a SQLite subquery source |

The suite validates every result against an independent Python oracle before
collecting timings. All engines consume every output cell in native code using
the same checksum sink; Python orchestrates processes outside the timed region.
The comparison measures the public query interfaces, including result transport.
It does not isolate just the accumulation kernel.

## Build

From the repository root, build a release Perfetto runner:

```sh
tools/gn gen out/linux_clang_release --args='is_debug=false'
tools/ninja -C out/linux_clang_release pipeline_benchmark_runner
```

If that build directory already has custom GN arguments, retain them and run
`tools/gn gen out/linux_clang_release` without replacing its arguments. The target
requires `enable_perfetto_benchmarks` and
`enable_perfetto_trace_processor_sqlite` (enabled in the normal Linux build).

Provide an official DuckDB C distribution containing `duckdb.h` and the shared
library. The helper never downloads dependencies. This run used the
[DuckDB v1.4.4 Linux amd64 release](https://github.com/duckdb/duckdb/releases/tag/v1.4.4):

```sh
mkdir -p /tmp/pipeline-duckdb
curl -fL https://github.com/duckdb/duckdb/releases/download/v1.4.4/libduckdb-linux-amd64.zip -o /tmp/pipeline-duckdb/duckdb.zip
printf '%s\n' '09cc288295964d897b47665d1898e16e8ef176cae9ea615797fc136eae15bd5d  /tmp/pipeline-duckdb/duckdb.zip' | sha256sum -c -
unzip /tmp/pipeline-duckdb/duckdb.zip -d /tmp/pipeline-duckdb
python3 benchmarks/pipeline/build_duckdb_runner.py \
  --duckdb-dir /tmp/pipeline-duckdb \
  --output out/pipeline-benchmark/duckdb_runner \
  --cxx buildtools/linux64/clang/bin/clang++
```

Keep the library directory in place: the runner links to that shared library.
The helper records compiler, source, header, binary and library hashes alongside
the executable. Both runner translation units use release optimization; the
DuckDB library itself is the official binary distribution, not a matched source
build. Other DuckDB versions can be supplied explicitly.

## Run

```sh
# Empty/singleton inputs, NULLs, awkward row orders and batch boundaries.
python3 benchmarks/pipeline/run.py --profile smoke \
  --repeat 1 --warmup 1 --output out/pipeline-results/smoke

# Representative shapes, sizes and composed stages; both timing modes.
python3 benchmarks/pipeline/run.py --profile quick \
  --repeat 9 --warmup 2 --cpu 0 --output out/pipeline-results/quick

# Focused scaling experiment.
python3 benchmarks/pipeline/run.py --rows 1000,10000,100000 \
  --shapes balanced --workloads scan,down,up \
  --repeat 9 --warmup 2 --cpu 0 --output out/pipeline-results/scaling

# Separate one-column, numeric-only and mixed-type scan transport costs.
python3 benchmarks/pipeline/run.py --rows 1000,100000 \
  --workloads scan_int,scan_numeric,scan --repeat 51 --warmup 5 \
  --cpu 0 --output out/pipeline-results/scans
```

Choose a CPU allowed by your environment, or omit `--cpu`. DuckDB defaults to one
thread. Do not run competing benchmarks or builds during measurement. Each output
directory must be new or empty. `--help` lists filters, runner paths, layouts and
resource limits. Explicit `--rows` builds a custom matrix; otherwise filters
select cases from the profile. `full` is a substantially larger matrix.

Each directory contains `summary.md`, `summary.csv`, `results.json`, generated SQL,
exact validation rows, native metadata, raw timing samples and process logs.
The JSON records machine, affinity, revision, dirty state, source/binary hashes,
GN arguments and DuckDB build metadata. Results are saved incrementally.
On Linux, the harness checks the actual DuckDB shared-library path and hash,
including loader overrides, against the helper's build manifest.

Add `--backends pipeline,sqlite,dataframe,duckdb,pipeline_sql_scan` to include
the optional SQL-source path. It measures delegation and adapter costs; it does
not imply native pipeline filter or grouping operators.

## Queries and semantics

The workloads cover full and narrow scans, upward and downward integer sums,
two sums in one stage, and two/three successive accumulation stages. Trees include
balanced, star, chain and small disjoint forest shapes. Layouts include sparse
permuted IDs with shuffled insertion order and dense IDs in topological order.
Inputs exercise negative values, NULLs, floating and UTF-8 string transport.

For example:

```sql
FROM tree |> TREE ACCUMULATE DOWN SUM(value) AS path;
FROM tree |> TREE ACCUMULATE UP SUM(value) AS total, SUM(weight) AS mass;
FROM tree |> TREE ACCUMULATE DOWN SUM(value) AS path
          |> TREE ACCUMULATE UP SUM(path) AS total;
```

The SQL DOWN translation recursively walks from roots and propagates path sums
in O(N) recursive rows. UP propagates descendant values toward their ancestors
and groups by ancestor, requiring the sum of node depths plus N recursive rows.
SQL sums use `COALESCE(value, 0)`, matching the pipeline's zero for all-NULL
aggregates. Integers stay within signed 64-bit range. This is not a floating-point
SUM benchmark; floating columns test result transport only. Generated query
files show the exact translations used for each case.

For SQLite backends, recursive DOWN traversal uses `CROSS JOIN` to keep the
recursive queue outside indexed child lookups. Without it, the dataframe planner
can rescan the entire table per node. Final joins stay optimizer-controlled:
forcing their order can repeatedly evaluate preceding stages. DuckDB uses normal
joins and its own optimizer.

All tables have ID and parent indexes where relevant. Ordinary SQLite uses an
`INTEGER PRIMARY KEY`, so it naturally clusters rows by ID. Dataframe staging
explicitly preserves the requested insertion order; it does not accidentally
inherit SQLite's ID ordering. DuckDB uses its own native storage and indexes.
These are natural storage layouts, not identical physical representations.
Creation, loading, finalization and index construction are outside query timing;
setup time is reported separately in native metadata.

## Measurement contract

* `end_to_end`: prepare/parse, execution, complete result consumption and statement
  destruction. Pipeline temporary-table cleanup deferred to the next query is
  included at the next preparation in steady state.
* `prepared`: reuse one prepared statement, reset/re-execute, and consume all
  results. Preparation is outside timing. Perfetto's public preparation path
  steps the first row; that initial execution is outside timing, and every
  measured execution starts again after reset.
* Warmups and repeats run in the same process and database. Each backend/mode
  gets a fresh process. Job order is seeded and shuffled; jobs run sequentially.
* Timing excludes process startup, setup and exact TSV validation. DuckDB's
  native query API materializes a result before chunk consumption; Perfetto's
  SQLite interface steps rows. Both are charged for their actual public APIs.
* Validation compares typed row multisets, including duplicates, NULLs, exact
  binary doubles and UTF-8 bytes. Output order is unspecified. Empty outputs also
  check column count. Every timed iteration checks count and checksum against
  its untimed execution; the checksum does not replace the independent oracle.
* `--timeout` bounds the **whole native process**, including setup, validation,
  all warmups and all repeats. It is not a per-query latency bound. SQL recursive
  work above `--max-recursive-rows` is explicitly skipped. Failures and skips are
  reported as such, never converted into speedup claims. A validation failure
  prevents timing that case.

Small scan differences should be treated as comparable unless repeat runs show
otherwise. Medians from this suite are machine- and workload-specific; they are
not general claims about SQLite or DuckDB. Long chains particularly favor the
pipeline's linear tree algorithm over the natural recursive UP translation.

## Tests

```sh
PIPELINE_PERFETTO_RUNNER="$PWD/out/linux_clang_release/pipeline_benchmark_runner" \
PIPELINE_DUCKDB_RUNNER="$PWD/out/pipeline-benchmark/duckdb_runner" \
  python3 -m unittest discover -s benchmarks/pipeline -p 'test_*.py'
```

Without runner environment variables, native integration tests skip. Tests cover
hand-derived answers, hundreds of generated SQLite/oracle comparisons, physical
input layout, exact output encoding, prepared resets, multiple batches, empty
schemas, malformed input, Unicode/NUL text and integer boundaries.
