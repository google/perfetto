# Union

**Purpose:** Combine rows from two or more sources into a single result set. All sources should have compatible column structures.

**How to use:**
- Connect multiple sources with similar column structures
- The node combines all rows from all inputs
- Columns are matched by name
- Missing columns in some inputs are filled with NULL values

**Data transformation:**
- All rows from all inputs are combined into one output
- Rows maintain their original data values
- The output contains the union of all columns from all inputs
- Rows are not deduplicated (same as SQL's UNION ALL)
- No particular ordering is guaranteed unless you add a Sort node after

**Example:** Combine slices from different tracks into a single view. Or merge events from multiple processes for analysis.

**Column handling:**
- If all inputs have the same columns, the output has those columns
- If inputs have different columns, the output has all columns, with NULL for missing values
- Column types should be compatible across inputs

**SQL equivalent:** `SELECT * FROM source1 UNION ALL SELECT * FROM source2 UNION ALL SELECT * FROM source3`
