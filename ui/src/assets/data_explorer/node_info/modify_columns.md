# Modify Columns

**Purpose:** Transform the column structure of your data by selecting, renaming, reordering, and changing column types and time units.

**How to use:**
- Check/uncheck columns to include or exclude them
- Add aliases to rename columns
- Drag columns to reorder them
- Click the type button to change column types (e.g., INT to STRING)
- For duration columns (`dur`), select time units (ns, us, ms, s, etc.) to convert values

**Data transformation:** All rows pass through unchanged. Only the column structure is modified - columns are selected, renamed, reordered, or have their types converted. The actual row data remains the same.

**Example:** Select only `id`, `ts`, and `dur` columns. Rename `ts` to `timestamp`. Convert `dur` from nanoseconds to milliseconds.
