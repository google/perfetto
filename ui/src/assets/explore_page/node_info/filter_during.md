# Filter During

**Purpose:** Filter rows from the left input to only those whose time interval (ts, dur) overlaps with any row from the right input's time interval. This is useful for finding events that occur during specific time windows.

**How to use:**
- Connect your main data source to the primary input (top port)
- Connect a time range or interval source to the secondary input (left port)
- Choose partitioning options to filter within specific groups (e.g., per process or thread)
- The node automatically filters rows based on time overlap.

**Data transformation:**
- Input rows are filtered to keep only those that temporally overlap with the right input
- If partitioning is enabled, filtering is done separately for each partition group
- Only rows from the left input are kept; the right input is used purely for filtering
- The timestamp and duration columns can be clipped to the ranges from interval intersect or they can be preserved. 
- All columns from the left input are preserved unchanged

**Example:** Filter slices to only show those that occurred during a specific time selection. Or find all thread slices that overlap with specific process execution windows.

**Time overlap logic:** A row overlaps if its interval `[ts, ts+dur)` has any intersection with an interval from the right input.
