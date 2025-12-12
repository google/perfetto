# Filter During

**Purpose:** Filter rows from the primary input to only those whose time interval (ts, dur) overlaps with any interval from the "Filter intervals" input. This is useful for finding events that occur during specific time windows.

**How to use:**
- Connect your main data source to the primary input (top port)
- Connect a time range or interval source to the "Filter intervals" input (port on the left)
- Choose partitioning options to filter within specific groups (e.g., per process or thread)
- The node automatically filters rows based on time overlap.

**Data transformation:**
- Input rows are filtered to keep only those that temporally overlap with the filter intervals
- If partitioning is enabled, filtering is done separately for each partition group
- Only rows from the primary input are kept; the filter intervals input is used purely for filtering
- The timestamp and duration columns can either be clipped to the intersection ranges or preserved.
- All columns from the primary input are preserved unchanged

**Example:** Filter slices to only show those that occurred during a specific time selection. Or find all thread slices that overlap with specific process execution windows.

**Time overlap logic:** A row overlaps if its interval `[ts, ts+dur)` has any intersection with an interval from the filter intervals input.
