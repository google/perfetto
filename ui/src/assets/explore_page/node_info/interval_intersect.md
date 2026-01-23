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

**Source selection:** By default, the output `ts` and `dur` columns come from the intersection result (cut to intersection boundaries), and there is no `id` column since the intersection doesn't correspond to a single source row. You can change this to use the original values from a specific input instead:
- **Intersection (no id):** Output `ts`/`dur` reflect the intersection boundaries, no `id` column (default)
- **Input N:** Output `id`/`ts`/`dur` come from that input's original intervals

This is useful when you want to filter intervals by overlap but preserve the original timing and identity from one of the sources.

**NOTE:** This node does not support unfinished slices ("did not terminate"). They will be filtered out from the intersection.
