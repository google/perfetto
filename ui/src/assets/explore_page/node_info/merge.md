# Merge

**Purpose:** Join two data sources together based on a condition. Combines rows where the join condition is met, bringing together columns from both sources.

**How to use:**
- Connect two data sources (left and right inputs)
- Choose a join condition:
  - **Equality condition**: Match rows where specific columns are equal (e.g., `left.id = right.id`)
  - **Freeform condition**: Write a custom SQL expression for more complex joins
- Specify query aliases to distinguish columns from each source

**Data transformation:**
- Rows from both inputs are joined based on the specified condition
- Creates one output row for each pair of matching rows
- Columns from both inputs are combined into each output row
- If the same column name exists in both sources, columns are prefixed with the query alias
- Only matching rows are included in the output (INNER JOIN behavior)

**Example 1 - Equality join:** Join slices with processes by matching `upid` column: `slices.upid = processes.upid`.

**Example 2 - Freeform join:** Join based on custom condition: `left.start_time < right.end_time AND left.end_time > right.start_time` to find overlapping intervals.

**Join types:** This node performs an INNER JOIN - only rows that match the condition are included in the output.

**SQL equivalent:** `SELECT * FROM left JOIN right ON <condition>`
