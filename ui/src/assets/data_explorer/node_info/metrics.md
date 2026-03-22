# Metrics

**Purpose:** Define trace-based metrics from your query results. This node packages your data into a `TraceMetricV2TemplateSpec` proto that can be exported and used in trace analysis pipelines. Each value column becomes a separate metric, and all remaining columns become dimensions.

**How to use:**

1. **Connect an input:** This node requires a source of data (e.g., from a Table Source or after filtering/aggregating data)

2. **Set Metric ID Prefix:** Give your metric a unique identifier prefix (e.g., `cpu_metrics`, `memory_usage`). Each metric is named `<prefix>_<column_name>`.

3. **Drag columns to Values:** The panel shows two lists — **Dimensions** on the left and **Values** on the right.
   - Drag any numeric column (int, double, etc.) from Dimensions to Values to make it a metric value.
   - Drag a value column back to Dimensions (or click ✕) to remove it.
   - Use the "Add value column" dropdown at the bottom of the Values panel as an alternative to drag-and-drop.

4. **Configure each Value column:** For each value column in the Values panel, set:
   - **Unit:** The appropriate unit (Count, Time, Bytes, Percentage, etc.). Use "Custom" to enter a custom unit string.
   - **Polarity:** Whether higher or lower values are "better":
     - *Higher is Better:* e.g., throughput, cache hit rate
     - *Lower is Better:* e.g., latency, error count
     - *Not Applicable:* when direction doesn't matter

5. **Configure Dimension Uniqueness:** Specify whether dimension combinations are unique:
   - *Unique:* Each combination of dimension values appears at most once
   - *Not Unique:* The same dimension combination may appear multiple times

**Dimensions:**
All columns **not** in the Values list automatically become dimensions. Use a Modify Columns node before this one to control which columns are present.

**Export:**
Click the "Export" button to generate a textproto representation of your metric template specification. A preview table shows the metric values as they would appear in an actual trace. When there are multiple value columns, each has its own tab in the preview.

**Example workflow:**
1. Start with a Table Source (e.g., `slice` table)
2. Add Aggregation to compute `SUM(dur)` and `COUNT(*)` grouped by `process_name`
3. Add a Metrics node:
   - Metric ID Prefix: `slice_stats`
   - Drag `sum_dur` to Values → Unit: Time (nanoseconds), Polarity: Lower is Better
   - Drag `count` to Values → Unit: Count, Polarity: Not Applicable
4. Export the metric spec

This creates two metrics — `slice_stats_sum_dur` and `slice_stats_count` — both with `process_name` as a dimension.

**Output:** The node passes through input columns unchanged. The metric template specification is generated separately via the Export button.
