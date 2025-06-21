# Trace Summarization

This guide explains how to use Perfetto's trace summarization feature to extract
structured, actionable data from your traces.

## Why Use Trace Summarization?

PerfettoSQL is a powerful tool for interactively exploring traces. You can write
any query you want, and the results are immediately available. However, this
flexibility presents a challenge for automation and large-scale analysis. The
output of a `SELECT` statement has an arbitrary schema (column names and types),
which can change from one query to the next. This makes it difficult to build
generic tools, dashboards, or regression-detection systems that consume this
data, as they cannot rely on a stable data structure.

**Trace summarization solves this problem.** It provides a way to define a
stable, structured schema for the data you want to extract from a trace. Instead
of producing arbitrary tables, it generates a consistent protobuf message
([`TraceSummary`](https://source.chromium.org/chromium/chromium/src/+/main:third_party/perfetto/protos/perfetto/trace_summary/file.proto;l=53?q=tracesummaryspec))
that is easy for tools to parse and process.

This is especially powerful for **cross-trace analysis**. By running the same
summary specification across hundreds or thousands of traces, you can reliably
aggregate the results to track performance metrics over time, compare different
versions of your application, and automatically detect regressions.

In short, use trace summarization when you need to:

- Extract data for automated tooling.
- Ensure a stable output schema for your analysis.
- Perform large-scale, cross-trace analysis.

## Using Summaries with the Standard Library

The easiest way to get started with trace summarization is by using the modules
available in the PerfettoSQL
[Standard Library](/docs/analysis/stdlib-docs.autogen).

Let's walk through an example. Suppose we want to compute the average memory
usage (specifically, RSS + Swap) for each process in a trace. The
`linux.memory.process` module already provides a table,
`memory_rss_and_swap_per_process`, that is perfect for this.

We can define a `TraceSummarySpec` to compute this metric:

```protobuf
// spec.textproto
metric_spec {
  id: "memory_per_process"
  dimensions: "process_name"
  value: "avg_rss_and_swap"
  query: {
    table: {
      table_name: "memory_rss_and_swap_per_process"
      module_name: "linux.memory.process"
    }
    group_by: {
      column_names: "process_name"
      aggregates: {
        column_name: "rss_and_swap"
        op: DURATION_WEIGHTED_MEAN
        result_column_name: "avg_rss_and_swap"
      }
    }
  }
}
```

To run this, save the above content as `spec.textproto` and use the
`trace_processor_shell`:

```bash
trace_processor_shell --summary \
  --summary-spec spec.textproto \
  --summary-metrics-v2 memory_per_process \
  my_trace.pftrace
```

The output will be a `TraceSummary` proto containing the results, which is
structured and ready for automated processing.

## Using Summaries with Custom SQL Modules

While the standard library is powerful, you will often need to analyze custom
events specific to your application. You can achieve this by writing your own
SQL modules and loading them into Trace Processor.

A SQL package is simply a directory containing `.sql` files. This directory can
be loaded into Trace Processor, and its files become available as modules.

Let's say you have custom slices named `game_frame` and you want to calculate
the average frame duration.

**1. Create your custom SQL module:**

Create a directory structure like this:

```
my_sql_modules/
└── my_game/
    └── metrics.sql
```

Inside `metrics.sql`, define a view that calculates the frame stats:

```sql
-- my_sql_modules/my_game/metrics.sql
CREATE PERFETTO VIEW game_frame_stats AS
SELECT
  'game_frame' AS frame_type,
  AVG(dur) AS avg_duration_ns
FROM slice
WHERE name = 'game_frame';
```

**2. Use the module in your summary spec:**

Now, create a `spec.textproto` that references your custom module and view:

```protobuf
// spec.textproto
metric_spec {
  id: "avg_game_frame_duration"
  dimensions: "frame_type"
  value: "avg_duration_ns"
  query: {
    table: {
      // The module name is the directory path relative to the package root,
      // with the .sql extension removed.
      module_name: "my_game.metrics"
      table_name: "game_frame_stats"
    }
  }
}
```

**3. Run the summary with your custom package:**

You can now compute the summary using either the Python API or the command-line
shell, telling Trace Processor where to find your custom package.

<?tabs>

TAB: Python API

Use the `add_sql_packages` argument in the `TraceProcessorConfig`.

```python
from perfetto.trace_processor import TraceProcessor, TraceProcessorConfig

# Path to your custom SQL modules directory
sql_package_path = './my_sql_modules'

config = TraceProcessorConfig(
    add_sql_packages=[sql_package_path]
)

with open('spec.textproto', 'r') as f:
    spec_text = f.read()

with TraceProcessor(trace='my_trace.pftrace', config=config) as tp:
    summary = tp.trace_summary(specs=[spec_text])
    print(summary)
```

TAB: Command-line shell

Use the `--add-sql-package` flag.

```bash
trace_processor_shell --summary \
  --add-sql-package ./my_sql_modules \
  --summary-spec spec.textproto \
  --summary-metrics-v2 avg_game_frame_duration \
  my_trace.pftrace
```

</tabs?>

## Common Patterns and Techniques

### Analyzing Time Intervals with `interval_intersect`

`interval_intersect` lets you analyze data from one source within specific time
windows defined by another source, ideal for analyzing "Critical User Journeys"
(CUJs).

It performs a time-based intersection of a primary data source (the `base`
query) with time intervals (the `interval_intersect` queries). An event from the
`base` query is included only if its time span overlaps with at least one
interval from _each_ `interval_intersect` query.

**Use Cases:**

- Calculate CPU usage of specific threads during defined CUJ periods.
- Analyze memory consumption of a process during a user interaction (defined by
  a slice).
- Find system events that occur only when multiple conditions are simultaneously
  true (e.g., "app in foreground" AND "scrolling activity").

#### Example: CPU Time during a Specific CUJ Slice

This example demonstrates using `interval_intersect` to find total CPU time for
thread `bar` within the duration of any "baz\_\*" slice from the "system_server"
process.

```protobuf
// In a metric_spec with id: "bar_cpu_time_during_baz_cujs"
query: {
  interval_intersect: {
     base: {
       // The base data is CPU time per thread.
       table: {
         table_name: "thread_slice_cpu_time"
         module_name: "linux.memory.process"
       }
       filters: {
         column_name: "thread_name"
         op: EQUAL
         string_rhs: "bar"
       }
     }
     interval_intersect: {
       // The intervals are the "baz_*" slices.
       simple_slices: {
         slice_name_glob: "baz_*"
         process_name_glob: "system_server"
       }
     }
  }
  group_by: {
    // We sum the CPU time from the intersected intervals.
    aggregates: {
      column_name: "cpu_time"
      op: SUM
      result_column_name: "total_cpu_time"
    }
  }
}
```

### Adding Trace-Wide Metadata

You can add key-value metadata to your summary to provide context for the
metrics, such as the device model or OS version. This is especially useful when
analyzing multiple traces, as it allows you to group or filter results based on
this metadata.

The metadata is computed alongside any metrics you request in the same run.

**1. Define the metadata query in your spec:**

This query must return "key" and "value" columns.

```protobuf
// In spec.textproto, alongside your metric_spec definitions
query {
  id: "device_info_query"
  sql {
    sql: "SELECT 'device_name' AS key, 'Pixel Test' AS value"
    column_names: "key"
    column_names: "value"
  }
}
```

**2. Run the summary with both metrics and metadata:**

When you run the summary, you specify both the metrics you want to compute and
the query to use for metadata.

<?tabs>

TAB: Python API

Pass both `metric_ids` and `metadata_query_id`:

```python
summary = tp.trace_summary(
    specs=[spec_text],
    metric_ids=["avg_game_frame_duration"],
    metadata_query_id="device_info_query"
)
```

TAB: Command-line shell

Use both `--summary-metrics-v2` and `--summary-metadata-query`:

```bash
trace_processor_shell --summary \\
  --summary-spec spec.textproto \\
  --summary-metrics-v2 avg_game_frame_duration \\
  --summary-metadata-query device_info_query \\
  my_trace.pftrace
```

</tabs?>

### Output Format

The result of a summary is a `TraceSummary` protobuf message. This message
contains a `metric_bundles` field, which is a list of `TraceMetricV2Bundle`
messages.

Each bundle can contain the results for one or more metrics that were computed
together. This is useful when you have multiple metrics that share the same
dimensions and query, as it avoids duplicating the dimension values in the
output.

#### Example Output

For the `memory_per_process` example, the output `TraceSummary` would contain a
`TraceMetricV2Bundle` like this:

```protobuf
# In TraceSummary's metric_bundles field:
metric_bundles {
  specs {
    id: "memory_per_process"
    dimensions: "process_name"
    value: "avg_rss_and_swap"
    # ... query details ...
  }
  row {
    values { double_value: 123456.789 }
    dimension { string_value: "com.example.app" }
  }
  row {
    values { double_value: 98765.432 }
    dimension { string_value: "system_server" }
  }
  # ...
}
```

## Comparison with the Legacy Metrics System

Perfetto previously had a different system for computing metrics, often referred
to as "v1 metrics." Trace summarization is the successor to this system,
designed to be more robust and easier to use.

Here are the key differences:

- **Output Schema**: The legacy system required users to define their own output
  protobuf schemas. This was powerful but had a steep learning curve and led to
  inconsistent, hard-to-maintain outputs. Trace summarization uses a single,
  well-defined output proto (`TraceSummary`), ensuring that all summaries are
  structured consistently.
- **Ease of Use**: With trace summarization, you do not need to write or manage
  any `.proto` files for the output. You only need to define _what_ data to
  compute (the query) and its _shape_ (dimensions and value). Perfetto handles
  the rest.
- **Flexibility vs. Tooling**: While the legacy system offered more flexibility
  in the output structure, this came at the cost of toolability. The
  standardized output of trace summarization makes it far easier to build
  reliable, long-term tools for analysis, visualization, and regression
  tracking.

## Reference

### Running Summaries

You can compute summaries using different Perfetto tools.

<?tabs>

TAB: Python API

For programmatic workflows, use the `trace_summary` method of the
`TraceProcessor` class.

```python
from perfetto.trace_processor.api import TraceProcessor

# Assume 'tp' is an initialized TraceProcessor instance
# and 'spec_text' contains your TraceSummarySpec.

summary_proto = tp.trace_summary(
    specs=[spec_text],
    metric_ids=["example_metric"],
    metadata_query_id="device_info_query"
)

print(summary_proto)
```

The `trace_summary` method takes the following arguments:

- **`specs`**: A list of `TraceSummarySpec` definitions (as text or bytes).
- **`metric_ids`**: An optional list of metric IDs to compute. If `None`, all
  metrics in the specs are computed.
- **`metadata_query_id`**: An optional ID of a query to run for trace-wide
  metadata.

TAB: Command-line shell

The `trace_processor_shell` allows you to compute trace summaries from a trace
file using dedicated flags.

- **Run specific metrics by ID:** Provide a comma-separated list of metric IDs
  using the `--summary-metrics-v2` flag.
  ```bash
  trace_processor_shell --summary \\
    --summary-spec YOUR_SPEC_FILE \\
    --summary-metrics-v2 METRIC_ID_1,METRIC_ID_2 \\
    TRACE_FILE
  ```
- **Run all metrics defined in the spec:** Use the keyword `all`.
  ```bash
  trace_processor_shell --summary \\
    --summary-spec YOUR_SPEC_FILE \\
    --summary-metrics-v2 all \\
    TRACE_FILE
  ```
- **Output Format:** Control the output format with `--summary-format`.
  - `text`: Human-readable text protobuf (default).
  - `binary`: Binary protobuf.

</tabs?>

### [`TraceSummarySpec`](/protos/perfetto/trace_summary/file.proto)

The top-level message for configuring a summary. It contains:

- **`metric_spec` (repeated
  [`TraceMetricV2Spec`](/protos/perfetto/trace_summary/v2_metric.proto))**:
  Defines individual metrics.
- **`query` (repeated
  [`PerfettoSqlStructuredQuery`](/protos/perfetto/perfetto_sql/structured_query.proto))**:
  Defines shared queries that can be referenced by metrics or used for
  trace-wide metadata.

### [`TraceSummary`](/protos/perfetto/trace_summary/file.proto)

The top-level message for the output of a summary. It contains:

- **`metric_bundles` (repeated
  [`TraceMetricV2Bundle`](/protos/perfetto/trace_summary/v2_metric.proto))**:
  The computed results for each metric.
- **`metadata` (repeated `Metadata`)**: Key-value pairs of trace-level metadata.

### [`TraceMetricV2Spec`](/protos/perfetto/trace_summary/v2_metric.proto)

Defines a single metric.

- **`id` (string)**: A unique identifier for the metric.
- **`dimensions` (repeated string)**: Columns that act as dimensions.
- **`value` (string)**: The column containing the metric's numerical value.
- **`query`
  ([`PerfettoSqlStructuredQuery`](/protos/perfetto/perfetto_sql/structured_query.proto))**:
  The query to compute the data.

### [`TraceMetricV2TemplateSpec`](/protos/perfetto/trace_summary/v2_metric.proto)

Defines a template for generating multiple, related metrics from a single,
shared configuration. This is useful for reducing duplication when you have
several metrics that share the same query and dimensions.

Using a template automatically bundles the generated metrics into a single
[`TraceMetricV2Bundle`](/protos/perfetto/trace_summary/v2_metric.proto) in the
output.

- **`id_prefix` (string)**: A prefix for the IDs of all generated metrics.
- **`dimensions` (repeated string)**: The shared dimensions for all metrics.
- **`value_columns` (repeated string)**: A list of columns from the query. Each
  column will generate a unique metric with the ID `<id_prefix>_<value_column>`.
- **`query`
  ([`PerfettoSqlStructuredQuery`](/protos/perfetto/perfetto_sql/structured_query.proto))**:
  The shared query that computes the data for all metrics.

### [`TraceMetricV2Bundle`](/protos/perfetto/trace_summary/v2_metric.proto)

Contains the results for one or more metrics which are bundled together.

- **`specs` (repeated `TraceMetricV2Spec`)**: The specs for all the metrics in
  the bundle.
- **`row` (repeated `Row`)**: Each row contains the dimension values and all the
  metric values for that set of dimensions.

### [`PerfettoSqlStructuredQuery`](/protos/perfetto/perfetto_sql/structured_query.proto)

The `PerfettoSqlStructuredQuery` message provides a structured way to define
PerfettoSQL queries. It is built by defining a data `source` and then optionally
applying `filters`, `group_by` operations, and `select_columns` transformations.

#### Query Sources

A query's source can be one of the following:

- **`table`**: A PerfettoSQL table or view.
- **`sql`**: An arbitrary SQL `SELECT` statement.
- **`simple_slices`**: A convenience for querying the `slice` table.
- **`inner_query`**: A nested structured query.
- **`inner_query_id`**: A reference to a shared structured query.
- **`interval_intersect`**: A time-based intersection of a `base` data source
  with one or more `interval` data sources.

#### Query Operations

These operations are applied sequentially to the data from the source:

- **`filters`**: A list of conditions to filter rows.
- **`group_by`**: Groups rows and applies aggregate functions.
- **`select_columns`**: Selects and optionally renames columns.
