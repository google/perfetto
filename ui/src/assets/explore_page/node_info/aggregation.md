# Aggregation

**Purpose:** Compute summary statistics like `COUNT`, `SUM`, `MIN`, `MAX`, `AVG`, `MEDIAN`, or `PERCENTILE`. Optionally group rows by one or more columns.

**How to use:**
- **Select GROUP BY columns:** Choose columns to group by (optional)
  - Each unique combination of group values creates one output row
  - Leave empty to aggregate across all rows

- **Add aggregation functions:** Create computed columns with aggregations
  - Select an operation (COUNT, SUM, AVG, etc.)
  - Choose a column to aggregate (not needed for COUNT(*))
  - Name the result column
  - For PERCENTILE, specify the percentile value (0-100)

**Data transformation:**
- Multiple input rows are combined into summary rows
- If GROUP BY is specified:
  - One output row per unique combination of group column values
  - GROUP BY columns are preserved
  - Other columns are replaced by aggregation results
- If no GROUP BY:
  - Single output row with aggregation results across all input rows
- Only GROUP BY columns and aggregation result columns appear in the output

**Example 1 - No grouping:** Calculate total duration and average duration across all slices:
- Aggregations: `SUM(dur)` AS `total_duration`, `AVG(dur)` AS `avg_duration`
- Result: Single row with totals

**Example 2 - With grouping:** Find average duration per slice name:
- GROUP BY: `name`
- Aggregation: `AVG(dur)` AS `avg_duration`
- Result: One row per unique slice name

**Example 3 - Multiple groups:** Count slices per process and thread:
- GROUP BY: `process_name`, `thread_name`
- Aggregation: `COUNT(*)` AS `slice_count`
- Result: One row per unique process/thread combination

**Available operations:**
- `COUNT(*)`: Count all rows (no column needed)
- `COUNT(col)`: Count non-NULL values in column
- `SUM(col)`: Sum of all values
- `MIN(col)`, `MAX(col)`: Minimum/maximum value
- `MEAN(col)`: Average (arithmetic mean)
- `MEDIAN(col)`: Median (50th percentile)
- `PERCENTILE(col, p)`: Custom percentile (e.g., 95th percentile)
- `DURATION_WEIGHTED_MEAN(col)`: Mean weighted by duration

**SQL equivalent:** `SELECT group_cols, AGG(col) AS result FROM input GROUP BY group_cols`
