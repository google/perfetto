# Metrics

**Purpose:** Define trace-based metrics from your query results. This node packages your data into a `TraceMetricV2TemplateSpec` proto (metric bundle) that can be exported and used in trace analysis pipelines. The selected value column becomes the metric value, and all other columns become dimensions.

**How to use:**

1. **Connect an input:** This node requires a source of data (e.g., from a Table Source or after filtering/aggregating data)

2. **Set Metric ID Prefix:** Give your metric a unique identifier prefix (e.g., `cpu_metrics`, `memory_usage`). The metric will be named `<prefix>_<column_name>`.

3. **Select a Value Column:** Choose a numeric column (int, double, etc.) that contains the metric value you want to track. Then configure:
   - **Unit:** Select the appropriate unit for the metric values (Count, Time, Bytes, Percentage, etc.). Use "Custom" for units not in the predefined list.
   - **Polarity:** Indicate whether higher or lower values are "better"
     - Higher is Better: e.g., throughput, cache hit rate
     - Lower is Better: e.g., latency, error count
     - Not Applicable: for metrics where direction doesn't apply

4. **Configure Dimension Uniqueness:** Specify whether dimension combinations are unique
   - Unique: Each combination of dimension values appears at most once
   - Not Unique: The same dimension combination may appear multiple times

**Dimensions:**
All columns in your input **except** the value column automatically become dimensions. Use a Modify Columns node before this one to control which columns are included as dimensions.

**Export:**
Click the "Export" button to generate a textproto representation of your metric template specification. This can be saved and used in trace analysis pipelines.

**Example workflow:**
1. Start with a Table Source (e.g., `slice` table)
2. Add Aggregation to compute `SUM(dur)` grouped by `process_name`
3. Add Metrics node:
   - Metric ID Prefix: `slice_stats`
   - Value column: `sum_dur` with Unit: Time (nanoseconds), Polarity: Not Applicable
4. Export the metric spec

This creates a metric `slice_stats_sum_dur` with `process_name` as a dimension.

**Output:** The node passes through input columns unchanged. The metric template specification is generated separately via the Export button.
