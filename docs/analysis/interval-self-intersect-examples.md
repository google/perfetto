# Interval Self-Intersect Examples

Practical examples demonstrating interval self-intersection with various partition strategies.

## Example 1: Raw Intersection Buckets (No Aggregation)

Get raw intersection buckets without any aggregation - just the time ranges and group IDs:

```sql
INCLUDE PERFETTO MODULE intervals.self_intersect;

-- Get raw intersection structure without aggregation
SELECT
  ts,
  dur,
  group_id
FROM interval_to_table!(
  interval_intersect!(
    interval_partition!(
      (SELECT id, ts, dur FROM slice WHERE dur > 0),
      (),
      ()
    ),
    ()  -- Empty aggregation list for raw buckets
  ),
  (ts, dur, group_id)
)
ORDER BY ts
LIMIT 20;
```

**Use case**: Get the raw intersection structure for further processing or when you don't need aggregations.

## Example 2: All Slices (No Partition) - Count Only

Find all overlapping slices across the entire trace:

```sql
INCLUDE PERFETTO MODULE intervals.self_intersect;

-- Find periods where multiple slices overlap
SELECT
  ts,
  dur,
  group_id,
  count AS overlapping_slices
FROM interval_to_table!(
  interval_intersect!(
    interval_partition!(
      (SELECT id, ts, dur FROM slice WHERE dur > 0),
      (),
      ()
    ),
    (interval_agg!(count, COUNT))
  ),
  (ts, dur, group_id, count)
)
WHERE count > 1  -- Only show overlapping regions
ORDER BY count DESC, dur DESC
LIMIT 20;
```

**Use case**: Find the most congested time periods in your trace.

## Example 2: All Slices with Sum Aggregation

Sum the depth values of overlapping slices:

```sql
INCLUDE PERFETTO MODULE intervals.self_intersect;

-- Find periods where multiple slices overlap with total depth
SELECT
  ts,
  dur,
  group_id,
  count AS overlapping_slices,
  sum_depth AS total_depth
FROM interval_to_table!(
  interval_intersect!(
    interval_partition!(
      (SELECT id, ts, dur, depth FROM slice WHERE dur > 0),
      (),
      (depth)
    ),
    (interval_agg!(count, COUNT), interval_agg!(depth, SUM))
  ),
  (ts, dur, group_id, count, sum_depth)
)
WHERE count > 1
ORDER BY sum_depth DESC, count DESC
LIMIT 20;
```

**Use case**: Find the most congested time periods weighted by depth.

## Example 3: Self-Intersect Per Process (upid) - Count Only

Analyze slice overlaps within each process:

```sql
INCLUDE PERFETTO MODULE intervals.self_intersect;

-- Find overlapping slices per process
SELECT
  upid,
  process.name AS process_name,
  ts,
  dur,
  group_id,
  count AS concurrent_slices
FROM interval_to_table!(
  interval_intersect!(
    interval_partition!(
      (SELECT id, ts, dur, upid FROM slice WHERE dur > 0),
      (upid),
      ()
    ),
    (interval_agg!(count, COUNT))
  ),
  (ts, dur, group_id, count, upid)
)
JOIN process USING (upid)
WHERE count > 2  -- More than 2 concurrent slices
ORDER BY upid, count DESC, dur DESC;
```

**Use case**: Identify processes with high concurrency/parallelism.

## Example 4: Self-Intersect Per Process with Average Depth

```sql
INCLUDE PERFETTO MODULE intervals.self_intersect;

-- Find overlapping slices per process with average depth
SELECT
  upid,
  process.name AS process_name,
  ts,
  dur,
  group_id,
  count AS concurrent_slices,
  avg_depth
FROM interval_to_table!(
  interval_intersect!(
    interval_partition!(
      (SELECT id, ts, dur, depth, upid FROM slice WHERE dur > 0),
      (upid),
      (depth)
    ),
    (interval_agg!(count, COUNT), interval_agg!(depth, AVG))
  ),
  (ts, dur, group_id, count, avg_depth, upid)
)
JOIN process USING (upid)
WHERE count > 2
ORDER BY upid, avg_depth DESC, count DESC;
```

**Use case**: Identify processes with high concurrency weighted by depth.

## Example 5: CPU Scheduling Self-Intersect with Sum Priority

Analyze CPU scheduling overlaps (should be none on same CPU, but useful for validation):

```sql
INCLUDE PERFETTO MODULE intervals.self_intersect;

-- Check for scheduling overlaps per CPU (should be 1 everywhere)
SELECT
  cpu,
  ts,
  dur,
  count AS concurrent_threads,
  sum_priority AS total_priority
FROM interval_to_table!(
  interval_intersect!(
    interval_partition!(
      (SELECT id, ts, dur, priority, cpu FROM sched WHERE dur > 0),
      (cpu),
      (priority)
    ),
    (interval_agg!(count, COUNT), interval_agg!(priority, SUM))
  ),
  (ts, dur, group_id, count, sum_priority, cpu)
)
WHERE count > 1  -- This would indicate a bug!
ORDER BY cpu, ts;
```

**Use case**: Validate trace integrity - should have no overlaps per CPU.

## Example 6: Memory Allocation Pressure - Total Size

Find periods of high concurrent allocations:

```sql
INCLUDE PERFETTO MODULE intervals.self_intersect;

-- Analyze memory allocation concurrency with total size
SELECT
  upid,
  process.name,
  ts,
  dur,
  count AS concurrent_allocations,
  sum_size / 1024 / 1024 AS total_mb
FROM interval_to_table!(
  interval_intersect!(
    interval_partition!(
      (SELECT id, ts, dur, size, upid FROM heap_profile_allocation WHERE dur > 0),
      (upid),
      (size)
    ),
    (interval_agg!(count, COUNT), interval_agg!(size, SUM))
  ),
  (ts, dur, group_id, count, sum_size, upid)
)
JOIN process USING (upid)
WHERE count >= 5  -- High concurrency
ORDER BY total_mb DESC
LIMIT 30;
```

**Use case**: Identify memory pressure hotspots by total allocation size.

## Example 7: Memory Allocation Pressure - Largest Allocation

```sql
INCLUDE PERFETTO MODULE intervals.self_intersect;

-- Find largest concurrent allocation
SELECT
  upid,
  process.name,
  ts,
  dur,
  count AS concurrent_allocations,
  max_size / 1024 / 1024 AS largest_mb
FROM interval_to_table!(
  interval_intersect!(
    interval_partition!(
      (SELECT id, ts, dur, size, upid FROM heap_profile_allocation WHERE dur > 0),
      (upid),
      (size)
    ),
    (interval_agg!(count, COUNT), interval_agg!(size, MAX))
  ),
  (ts, dur, group_id, count, max_size, upid)
)
JOIN process USING (upid)
WHERE count >= 5
ORDER BY largest_mb DESC
LIMIT 30;
```

**Use case**: Identify memory pressure hotspots by largest allocation.

## Example 8: Binder Transaction Overlaps - Count Only

Analyze concurrent binder transactions:

```sql
INCLUDE PERFETTO MODULE intervals.self_intersect;

-- Find overlapping binder transactions per process
SELECT
  upid,
  process.name,
  ts,
  dur,
  count AS concurrent_binder_calls
FROM interval_to_table!(
  interval_intersect!(
    interval_partition!(
      (
        SELECT
          id,
          ts,
          dur,
          upid
        FROM slice
        WHERE name GLOB 'binder*' AND dur > 0
      ),
      (upid),
      ()
    ),
    (interval_agg!(count, COUNT))
  ),
  (ts, dur, group_id, count, upid)
)
JOIN process USING (upid)
WHERE count > 2
ORDER BY count DESC, dur DESC;
```

**Use case**: Identify binder bottlenecks and concurrent IPC patterns.

## Example 9: Frame Rendering Overlaps with Max Vsync ID

Analyze overlapping frame rendering work:

```sql
INCLUDE PERFETTO MODULE intervals.self_intersect;

-- Find overlapping frame rendering slices
SELECT
  upid,
  process.name,
  ts,
  dur,
  count AS concurrent_frames,
  max_vsync_id
FROM interval_to_table!(
  interval_intersect!(
    interval_partition!(
      (
        SELECT
          id,
          ts,
          dur,
          CAST(EXTRACT_ARG(arg_set_id, 'vsync_id') AS DOUBLE) AS vsync_id,
          upid
        FROM slice
        WHERE name = 'Choreographer#doFrame' AND dur > 0
      ),
      (upid),
      (vsync_id)
    ),
    (interval_agg!(count, COUNT), interval_agg!(vsync_id, MAX))
  ),
  (ts, dur, group_id, count, max_vsync_id, upid)
)
JOIN process USING (upid)
WHERE count > 1  -- Overlapping frames
ORDER BY count DESC;
```

**Use case**: Detect frame pacing issues and concurrent frame rendering.

## Example 10: Network Request Concurrency - Total Bytes

Analyze concurrent network requests:

```sql
INCLUDE PERFETTO MODULE intervals.self_intersect;

-- Analyze network request concurrency per process
SELECT
  upid,
  process.name,
  ts,
  dur,
  count AS concurrent_requests,
  sum_bytes / 1024 AS total_kb
FROM interval_to_table!(
  interval_intersect!(
    interval_partition!(
      (
        SELECT
          id,
          ts,
          dur,
          CAST(EXTRACT_ARG(arg_set_id, 'bytes') AS DOUBLE) AS bytes,
          upid
        FROM slice
        WHERE name GLOB 'http*' AND dur > 0
      ),
      (upid),
      (bytes)
    ),
    (interval_agg!(count, COUNT), interval_agg!(bytes, SUM))
  ),
  (ts, dur, group_id, count, sum_bytes, upid)
)
JOIN process USING (upid)
WHERE count >= 3
ORDER BY concurrent_requests DESC, total_kb DESC;
```

**Use case**: Understand network concurrency patterns by total bandwidth.

## Example 11: Maximum RSS Memory Across All Processes

Find time periods with the highest anonymous RSS memory usage across all processes:

```sql
INCLUDE PERFETTO MODULE intervals.self_intersect;
INCLUDE PERFETTO MODULE counters.intervals;

-- Convert RSS anon counters to intervals and find max across all processes
WITH rss_counters AS (
  SELECT
    c.id,
    c.ts,
    c.track_id,
    c.value
  FROM counter c
  JOIN process_counter_track t ON c.track_id = t.id
  WHERE t.name = 'mem.rss.anon'
)
SELECT
  ts,
  dur,
  group_id,
  max_value / 1024 / 1024 AS max_rss_mb,
  upid,
  process.name AS process_name
FROM interval_to_table!(
  interval_intersect!(
    interval_partition!(
      (
        SELECT
          intervals.id,
          intervals.ts,
          intervals.dur,
          intervals.value,
          t.upid
        FROM counter_leading_intervals!(rss_counters) intervals
        JOIN process_counter_track t ON intervals.id = t.id
        WHERE intervals.dur > 0
      ),
      (upid),
      (value)
    ),
    (interval_agg!(value, MAX))
  ),
  (ts, dur, group_id, max_value, upid)
)
JOIN process USING (upid)
ORDER BY max_rss_mb DESC
LIMIT 20;
```

**Use case**: Identify time periods and processes with peak anonymous RSS memory usage.

## Example 12: Thread State Self-Intersect - Validation

Analyze overlapping thread states (useful for detecting trace issues):

```sql
INCLUDE PERFETTO MODULE intervals.self_intersect;

-- Check for overlapping thread states per thread (should be 1)
SELECT
  utid,
  thread.name,
  ts,
  dur,
  count AS overlapping_states
FROM interval_to_table!(
  interval_intersect!(
    interval_partition!(
      (SELECT id, ts, dur, utid FROM thread_state WHERE dur > 0),
      (utid),
      ()
    ),
    (interval_agg!(count, COUNT))
  ),
  (ts, dur, group_id, count, utid)
)
JOIN thread USING (utid)
WHERE count > 1  -- This indicates a trace problem
ORDER BY utid, ts;
```

**Use case**: Validate trace integrity - thread states shouldn't overlap.

## Performance Tips

### 1. Filter Before Self-Intersect

```sql
-- GOOD: Filter first
interval_partition!(
  (SELECT id, ts, dur, upid FROM slice WHERE dur > 1000000),  -- Only long slices
  (upid),
  ()
)

-- BAD: Filter after
interval_partition!(
  (SELECT id, ts, dur, upid FROM slice),
  (upid),
  ()
)
-- Then filtering with WHERE dur > 1000000 is less efficient
```

### 2. Use Appropriate Partitions

```sql
-- GOOD: Partition by high-cardinality column
interval_partition!(
  slice_data,
  (upid),  -- ~100s of processes
  ()
)

-- LESS EFFICIENT: No partition with many intervals
interval_partition!(
  slice_data,
  (),  -- All intervals in one partition
  ()
)
```

### 3. Choose the Right Aggregation Functions

```sql
-- Use specific aggregations for what you need:
interval_agg!(count, COUNT)     -- Count only
interval_agg!(value, SUM)       -- Sum of values
interval_agg!(priority, MAX)    -- Maximum value
interval_agg!(size, MIN)        -- Minimum value
interval_agg!(depth, AVG)       -- Average value

-- You can combine multiple aggregations:
(interval_agg!(count, COUNT), interval_agg!(value, SUM), interval_agg!(priority, MAX))
```

## API Summary

### Core Macros

1. **`interval_partition!(table, (partition_cols), (agg_cols))`**
   - Creates a partitioned interval set for count-only or unaggregated queries
   - Example: `interval_partition!(my_table, (upid), ())`
   - Example: `interval_partition!(my_table, (), ())` for no partitioning
   - Example: `interval_partition!(my_table, (upid), (value))` for partitioning and aggregation

3. **`interval_agg!(column, AGG_TYPE)`**
   - Creates an aggregation specification
   - Supported types: COUNT, SUM, MIN, MAX, AVG
   - Example: `interval_agg!(value, SUM)` produces `sum_value` column
   - Example: `interval_agg!(priority, MAX)` produces `max_priority` column

4. **`interval_intersect!(partitions, (agg_specs))`**
   - Computes self-intersections with specified aggregations
   - Example: `interval_intersect!(parts, (interval_agg!(count, COUNT)))`
   - Example: `interval_intersect!(parts, (interval_agg!(count, COUNT), interval_agg!(value, SUM)))`
   - Example: `interval_intersect!(parts, ())` for unaggregated raw buckets

5. **`interval_to_table!(partitions, (output_columns))`**
   - Converts partitioned intervals back to a table
   - Always includes: `ts, dur, group_id`
   - Add aggregation columns based on interval_agg! specs
   - Add partition columns at the end
   - Example: `interval_to_table!(result, (ts, dur, group_id))` for unaggregated
   - Example: `interval_to_table!(result, (ts, dur, group_id, count, upid))`
   - Example: `interval_to_table!(result, (ts, dur, group_id, count, sum_value, upid))`

### Output Column Naming

Aggregation results are automatically named with prefixes:
- `COUNT` → `count`
- `SUM` → `sum_<column_name>`
- `MIN` → `min_<column_name>`
- `MAX` → `max_<column_name>`
- `AVG` → `avg_<column_name>`

### Constraints

- **Maximum 1 aggregation column** per query
- **Maximum 1 partition column** per query
- Partition column can be omitted: `()`
- Input table must have: `id, ts, dur` columns
- For aggregation functions, aggregation column must be numeric

## Comparison: Self-Intersect vs Span Join

| Feature | Self-Intersect | Span Join |
|---------|---------------|-----------|
| **Use Case** | Overlaps within ONE table | Overlaps between TWO tables |
| **Performance** | O(n log n) sweep line | O(n log n) |
| **Aggregations** | Built-in (count, sum, etc.) | Manual via GROUP BY |
| **Partitioning** | 1 column max | Via PARTITIONED keyword |
| **Output** | Buckets with group_id | Intersected intervals |

**When to use Self-Intersect**:
- Analyzing concurrency within a single dataset
- Need aggregations across overlapping intervals
- Want to identify time periods by overlap count

**When to use Span Join**:
- Correlating two different interval sets
- Need to preserve individual interval IDs
- Building complex interval relationships
- Need multi-column partitioning
