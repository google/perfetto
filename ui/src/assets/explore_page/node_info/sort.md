# Sort

**Purpose:** Reorder rows by one or more columns in ascending or descending order.

**How to use:**
- Select columns to sort by
- Choose ascending (A→Z, 1→9) or descending (Z→A, 9→1) order for each column
- Multiple sort columns create hierarchical ordering (first column, then second, etc.)
- Drag to reorder sort priority

**Data transformation:**
- All rows pass through unchanged
- Only the order of rows changes
- All columns are preserved
- Rows with NULL values typically appear first (ascending) or last (descending)

**Example:** Sort slices by `dur` descending to see longest durations first. Or sort by `process_name` then `ts` to see events chronologically within each process.

**Sort order:** When sorting by multiple columns, rows are first sorted by the first column, then ties are broken by the second column, and so on.

**SQL equivalent:** `ORDER BY column1 ASC, column2 DESC`
