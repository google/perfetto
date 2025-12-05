# Interval Intersect

**Purpose:** Compute the intersection of time intervals between two sources. For each pair of overlapping intervals, this node creates a new row with the intersected time range.

**How to use:**
- Connect two sources that have time interval columns (ts, dur)
- The node finds all pairs of overlapping intervals
- Creates one output row for each overlap, with the intersection time range

**Data transformation:**
- Each output row represents where two input intervals overlap
- The output `ts` is the maximum of the two input start times
- The output `dur` is the length of the overlap
- Columns from both inputs are included (with `_left` and `_right` suffixes for conflicts)
- Only creates rows where intervals actually overlap (non-overlapping intervals are excluded)

**Example:** Find the overlaps between GPU and CPU activity. Or compute when two different types of events occur simultaneously.

**Intersection logic:** Two intervals overlap if `[ts1, ts1+dur1)` intersects with `[ts2, ts2+dur2)`. The output is the intersection `[max(ts1, ts2), min(ts1+dur1, ts2+dur2))`.
