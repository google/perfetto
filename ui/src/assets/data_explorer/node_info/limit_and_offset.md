# Limit and Offset

**Purpose:** Control how many rows to return and where to start in the result set. Useful for pagination or limiting output size.

**How to use:**
- Set **Limit** to specify the maximum number of rows to return
- Set **Offset** to skip a number of rows from the beginning
- Leave either empty to apply no limit/offset
- Often used with Sort node to get "top N" results

**Data transformation:**
- Rows are passed through unchanged, but only a subset is included in the output
- **Offset** rows are skipped from the start
- Then up to **Limit** rows are returned
- All columns are preserved unchanged
- The order of rows matters - use Sort node before this to control which rows are kept

**Example:** Get the top 10 longest slices: Sort by `dur` descending, then Limit to 10. Or implement pagination: Limit 100, Offset 200 for page 3.

**SQL equivalent:** `LIMIT {limit} OFFSET {offset}`
