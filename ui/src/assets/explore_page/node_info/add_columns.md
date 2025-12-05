# Add Columns

**Purpose:** Enrich your data by adding columns from another table or query, or by creating computed columns with expressions.

**How to use:**
- **From another source:** Connect an additional data source to the left port
  - Specify which columns to match (join key)
  - Select which columns to add from the connected source
  - Rename columns with aliases to avoid conflicts

- **Expression:** Create computed columns using SQL expressions
  - Example: `dur / 1e6` to convert duration to milliseconds

- **Switch:** Create conditional columns with SWITCH/CASE logic
  - Select a column to switch on
  - Define cases and values
  - Optionally use GLOB matching for patterns

- **If:** Create conditional columns with IF/THEN/ELSE logic
  - Define conditions and values
  - Chain multiple conditions with ELSE IF

- **From args:** Extract argument values into columns
  - Select an `arg_set_id` column
  - Choose an arg key to extract
  - The value becomes a new column

**Data transformation:**
- All rows from the primary input are preserved
- New columns are added to each row:
  - From JOIN: Values are looked up from the connected source using the join key
  - From expressions: Computed for each row
  - From SWITCH/IF: Evaluated based on conditions
  - From args: Extracted from the args table
- Existing columns are not modified

**Example 1 - JOIN:** Add process details to slices by joining on `upid` with the process table.

**Example 2 - Expression:** Add `dur_ms` column with expression `dur / 1e6`.

**Example 3 - Switch:** Create a `priority` column: SWITCH ON `name`, WHEN `'render'` THEN `'high'`, DEFAULT `'normal'`.

**Join types:** This node performs a LEFT JOIN - all rows from the primary input are kept, even if there's no match in the connected source (unmatched columns will be NULL).
