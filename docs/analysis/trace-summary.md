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

The easiest way to get started is by using the modules in the
[PerfettoSQL Standard Library](/docs/analysis/stdlib-docs.autogen).

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
    }
    referenced_modules: "linux.memory.process"
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

To run this, save the above content as `spec.textproto` and use your preferred
tool.

<?tabs>

TAB: Python API

```python
from perfetto.trace_processor import TraceProcessor

with open('spec.textproto', 'r') as f:
    spec_text = f.read()

with TraceProcessor(trace='my_trace.pftrace') as tp:
    summary = tp.trace_summary(
        specs=[spec_text],
        metric_ids=["memory_per_process"]
    )
    print(summary)
```

TAB: Command-line shell

```bash
trace_processor_shell --summary \
  --summary-spec spec.textproto \
  --summary-metrics-v2 memory_per_process \
  my_trace.pftrace
```

</tabs?>

## Reducing Duplication with Templates

Often, you'll want to compute several related metrics that share the same
underlying query and dimensions. For example, for a given process, you might
want to know the minimum, maximum, and average memory usage.

Instead of writing a separate `metric_spec` for each, which would involve
repeating the same `query` and `dimensions` blocks, you can use a
[`TraceMetricV2TemplateSpec`](/protos/perfetto/trace_summary/v2_metric.proto).
This is more concise, less error-prone, and more performant as the underlying
query is only run once.

Let's extend our memory example to calculate the min, max, and duration-weighted
average of RSS+Swap for each process.

```protobuf
// spec.textproto
metric_template_spec {
  id_prefix: "memory_per_process"
  dimensions: "process_name"
  value_columns: "min_rss_and_swap"
  value_columns: "max_rss_and_swap"
  value_columns: "avg_rss_and_swap"
  query: {
    table: {
      table_name: "memory_rss_and_swap_per_process"
    }
    referenced_modules: "linux.memory.process"
    group_by: {
      column_names: "process_name"
      aggregates: {
        column_name: "rss_and_swap"
        op: MIN
        result_column_name: "min_rss_and_swap"
      }
      aggregates: {
        column_name: "rss_and_swap"
        op: MAX
        result_column_name: "max_rss_and_swap"
      }
      aggregates: {
        column_name: "rss_and_swap"
        op: DURATION_WEIGHTED_MEAN
        result_column_name: "avg_rss_and_swap"
      }
    }
  }
}
```

This single template generates three metrics:

- `memory_per_process_min_rss_and_swap`
- `memory_per_process_max_rss_and_swap`
- `memory_per_process_avg_rss_and_swap`

You can then run this, requesting any or all of the generated metrics, as shown
below.

<?tabs>

TAB: Python API

```python
from perfetto.trace_processor import TraceProcessor

with open('spec.textproto', 'r') as f:
    spec_text = f.read()

with TraceProcessor(trace='my_trace.pftrace') as tp:
    summary = tp.trace_summary(
        specs=[spec_text],
        metric_ids=[
            "memory_per_process_min_rss_and_swap",
            "memory_per_process_max_rss_and_swap",
            "memory_per_process_avg_rss_and_swap",
        ]
    )
    print(summary)
```

TAB: Command-line shell

```bash
trace_processor_shell --summary \
  --summary-spec spec.textproto \
  --summary-metrics-v2 memory_per_process_min_rss_and_swap,memory_per_process_max_rss_and_swap,memory_per_process_avg_rss_and_swap \
  my_trace.pftrace
```

</tabs?>

## Adding Units and Polarity

To make automated analysis and visualization of metrics more powerful, you can
add units and polarity (i.e., whether a higher or lower value is better) to your
metrics.

This is done by using the `value_column_specs` field in a
`TraceMetricV2TemplateSpec` instead of the simpler `value_columns`. This allows
you to specify a `unit` and `polarity` for each metric generated by the
template.

Let's adapt our previous memory example to include this information. We'll
specify that the memory values are in `BYTES` and that a lower value is
better.

```protobuf
// spec.textproto
metric_template_spec {
  id_prefix: "memory_per_process"
  dimensions: "process_name"
  value_column_specs: {
    name: "min_rss_and_swap"
    unit: BYTES
    polarity: LOWER_IS_BETTER
  }
  value_column_specs: {
    name: "max_rss_and_swap"
    unit: BYTES
    polarity: LOWER_IS_BETTER
  }
  value_column_specs: {
    name: "avg_rss_and_swap"
    unit: BYTES
    polarity: LOWER_IS_BETTER
  }
  query: {
    table: {
      table_name: "memory_rss_and_swap_per_process"
    }
    referenced_modules: "linux.memory.process"
    group_by: {
      column_names: "process_name"
      aggregates: {
        column_name: "rss_and_swap"
        op: MIN
        result_column_name: "min_rss_and_swap"
      }
      aggregates: {
        column_name: "rss_and_swap"
        op: MAX
        result_column_name: "max_rss_and_swap"
      }
      aggregates: {
        column_name: "rss_and_swap"
        op: DURATION_WEIGHTED_MEAN
        result_column_name: "avg_rss_and_swap"
      }
    }
  }
}
```

This will add the specified `unit` and `polarity` to the `TraceMetricV2Spec` of
each generated metric, making the output richer and more useful for automated
tooling.

## Using Summaries with Custom SQL Modules

While the standard library is powerful, you will often need to analyze custom
events specific to your application. You can achieve this by writing your own
SQL modules and loading them into Trace Processor.

A SQL package is simply a directory containing `.sql` files. This directory can
be loaded into Trace Processor, and its files become available as modules.

Let's say you have custom slices named `game_frame` and you want to calculate
the average, minimum, and maximum frame duration.

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
  MIN(dur) AS min_duration_ns,
  MAX(dur) AS max_duration_ns,
  AVG(dur) AS avg_duration_ns
FROM slice
WHERE name = 'game_frame'
GROUP BY 1;
```

**2. Use a template in your summary spec:**

Again, we can use a `TraceMetricV2TemplateSpec` to generate these related
metrics from a single, shared configuration.

Create a `spec.textproto` that references your custom module and view:

```protobuf
// spec.textproto
metric_template_spec {
  id_prefix: "game_frame"
  dimensions: "frame_type"
  value_columns: "min_duration_ns"
  value_columns: "max_duration_ns"
  value_columns: "avg_duration_ns"
  query: {
    table: {
      // The module name is the directory path relative to the package root,
      // with the .sql extension removed.
      table_name: "game_frame_stats"
    }
    referenced_modules: "my_game.metrics"
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
    # Requesting one, some, or all of the generated metrics.
    summary = tp.trace_summary(
        specs=[spec_text],
        metric_ids=[
            "game_frame_min_duration_ns",
            "game_frame_max_duration_ns",
            "game_frame_avg_duration_ns"
        ]
    )
    print(summary)
```

TAB: Command-line shell

Use the `--add-sql-package` flag. You can list the metrics explicitly or use
the `all` keyword.

```bash
trace_processor_shell --summary \
  --add-sql-package ./my_sql_modules \
  --summary-spec spec.textproto \
  --summary-metrics-v2 game_frame_min_duration_ns,game_frame_max_duration_ns,game_frame_avg_duration_ns \
  my_trace.pftrace
```

</tabs?>

## Common Patterns and Techniques

### Column Transformations

The `select_columns` field provides a powerful way to manipulate the columns of
your query result. You can rename columns and perform transformations using SQL
expressions.

Each `SelectColumn` message has two fields:

-   `column_name_or_expression`: The name of a column from the source or a SQL
    expression.
-   `alias`: The new name for the column.

#### Example: Renaming and Transforming Columns

This example shows how to select the `ts` and `dur` columns from the `slice`
table, rename `ts` to `timestamp`, and create a new column `dur_ms` by
converting `dur` from nanoseconds to milliseconds.

```protobuf
query: {
  table: {
    table_name: "slice"
  }
  select_columns: {
    column_name_or_expression: "ts"
    alias: "timestamp"
  }
  select_columns: {
    column_name_or_expression: "dur / 1000"
    alias: "dur_ms"
  }
}
```

### Analyzing Time Intervals with `interval_intersect`

A common analysis pattern is to analyze data from one source (e.g., CPU usage)
within specific time windows from another (e.g., a "Critical User Journey"
slice). The `interval_intersect` query makes this easy.

It works by taking a `base` query and one or more `interval` queries. The result
includes only the rows from the `base` query that overlap in time with at least
one row from _each_ of the `interval` queries.

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
       }
       referenced_modules: "slices.cpu_time"
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

### Composing Queries with `dependencies`

The `dependencies` field in the `Sql` source allows you to build complex
queries by composing them from other structured queries. This is especially
useful for breaking down a complex analysis into smaller, reusable parts.

Each dependency is given an `alias`, which is a string that can be used in the
SQL query to refer to the result of the dependency. The SQL query can then
use this alias as if it were a table.

#### Example: Joining CPU data with CUJ slices

This example shows how to use `dependencies` to join CPU scheduling data
with CUJ slices. We define two dependencies, one for the CPU data and one for
the CUJ slices, and then join them in the main SQL query.

```protobuf
query: {
  sql: {
    sql: "SELECT s.id, s.ts, s.dur, t.track_name FROM $slice_table s JOIN $track_table t ON s.track_id = t.id"
    column_names: "id"
    column_names: "ts"
    column_names: "dur"
    column_names: "track_name"
    dependencies: {
      alias: "slice_table"
      query: {
        table: {
          table_name: "slice"
        }
      }
    }
    dependencies: {
      alias: "track_table"
      query: {
        table: {
          table_name: "track"
        }
      }
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
    metric_ids=["game_frame_avg_duration_ns"],
    metadata_query_id="device_info_query"
)
```

TAB: Command-line shell

Use both `--summary-metrics-v2` and `--summary-metadata-query`:

```bash
trace_processor_shell --summary \\
  --summary-spec spec.textproto \\
  --summary-metrics-v2 game_frame_avg_duration_ns \\
  --summary-metadata-query device_info_query \\
  my_trace.pftrace
```

</tabs?>

### Output Format

The result of a summary is a `TraceSummary` protobuf message. This message
contains a `metric_bundles` field, which is a list of `TraceMetricV2Bundle`
messages.

Each bundle can contain the results for one or more metrics that were computed
together. Using a `TraceMetricV2TemplateSpec` is the most common way to create a
bundle. All metrics generated from a single template are automatically placed in
the same bundle, sharing the same `specs` and `row` structure. This is highly
efficient as the dimension values, which are often repetitive, are only written
once per row.

#### Example Output

For the `memory_per_process` template example, the output `TraceSummary` would
contain a `TraceMetricV2Bundle` like this:

```protobuf
# In TraceSummary's metric_bundles field:
metric_bundles {
  # The specs for all three metrics generated by the template.
  specs {
    id: "memory_per_process_min_rss_and_swap"
    dimensions: "process_name"
    value: "min_rss_and_swap"
    # ... query details ...
  }
  specs {
    id: "memory_per_process_max_rss_and_swap"
    dimensions: "process_name"
    value: "max_rss_and_swap"
    # ... query details ...
  }
  specs {
    id: "memory_per_process_avg_rss_and_swap"
    dimensions: "process_name"
    value: "avg_rss_and_swap"
    # ... query details ...
  }
  # Each row contains one set of dimensions and three values, corresponding
  # to the three metrics in `specs`.
  row {
    values { double_value: 100000 } # min
    values { double_value: 200000 } # max
    values { double_value: 123456.789 } # avg
    dimension { string_value: "com.example.app" }
  }
  row {
    values { double_value: 80000 } # min
    values { double_value: 150000 } # max
    values { double_value: 98765.432 } # avg
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
from perfetto.trace_processor import TraceProcessor

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
- **`unit` (oneof)**: The unit of the metric's value (e.g. `TIME_NANOS`, `BYTES`). Can also be a `custom_unit` string.
- **`polarity` (enum)**: Whether a higher or lower value is better (e.g. `HIGHER_IS_BETTER`, `LOWER_IS_BETTER`).
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
- **`value_column_specs` (repeated `ValueColumnSpec`)**: A list of value column
  specifications, allowing each to have a unique `unit` and `polarity`.
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

#### Aggregation Operators

The `group_by` operation allows you to use the following aggregate functions:

| Operator                 | Description                                                                                                                                            |
| ------------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `COUNT`                  | Counts the number of rows in each group. If no `column_name` is specified, this becomes `COUNT(*)` (count all rows). |
| `SUM`                    | Calculates the sum of a numerical column.                                                                                                              |
| `MIN`                    | Finds the minimum value of a numerical column.                                                                                                         |
| `MAX`                    | Finds the maximum value of a numerical column.                                                                                                         |
| `MEAN`                   | Calculates the average value of a numerical column.                                                                                                    |
| `MEDIAN`                 | Calculates the 50th percentile of a numerical column.                                                                                                  |
| `DURATION_WEIGHTED_MEAN` | Calculates the duration-weighted average of a numerical column. This is useful for time-series data where values should be weighted by their duration. |
| `PERCENTILE`             | Calculates a given percentile of a numerical column. The percentile is specified in the `percentile` field of the `Aggregate` message.                   |

##### Aggregation Field Requirements

- **`COUNT`**: `column_name` is optional. If omitted, it defaults to `COUNT(*)`.
- **`SUM`, `MIN`, `MAX`, `MEAN`, `MEDIAN`, `DURATION_WEIGHTED_MEAN`**: `column_name` is required.
- **`PERCENTILE`**: Both `column_name` and `percentile` are required.

##### Example: Calculating the 99th Percentile

This example shows how to calculate the 99th percentile of the `dur` column from the `slice` table.

```protobuf
query: {
  table: {
    table_name: "slice"
  }
  group_by: {
    aggregates: {
      column_name: "dur"
      op: PERCENTILE
      result_column_name: "p99_dur"
      percentile: 99
    }
  }
}
```
