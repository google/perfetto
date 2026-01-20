# Interval Self-Intersect Examples

Practical examples demonstrating interval self-intersection with various partition strategies.

**Note**: The current implementation supports **1 aggregation column** and **at most 1 partition column** for optimal performance.

## Example 1: All Slices (No Partition) - Count Only

Find all overlapping slices across the entire trace:

```sql
INCLUDE PERFETTO MODULE intervals.self_intersect;

-- Find periods where multiple slices overlap
SELECT
  ts,
  dur,
  group_id,
  count AS overlapping_slices
FROM _interval_self_intersect(
  (SELECT id, ts, dur FROM slice WHERE dur > 0),
  ()
)
WHERE count > 1  -- Only show overlapping regions
ORDER BY count DESC, dur DESC
LIMIT 20;
```

**Use case**: Find the most congested time periods in your trace.

## Example 1b: All Slices with Sum Aggregation

Sum the depth values of overlapping slices:

```sql
INCLUDE PERFETTO MODULE intervals.self_intersect;

-- Find periods where multiple slices overlap with total depth
SELECT
  ts,
  dur,
  group_id,
  count AS overlapping_slices,
  sum AS total_depth
FROM _interval_self_intersect_sum(
  (SELECT id, ts, dur, depth FROM slice WHERE dur > 0),
  (),
  depth
)
WHERE count > 1
ORDER BY sum DESC, count DESC
LIMIT 20;
```

**Use case**: Find the most congested time periods weighted by depth.

## Example 2: Self-Intersect Per Process (upid) - Count Only

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
FROM _interval_self_intersect(
  (SELECT id, ts, dur, upid FROM slice WHERE dur > 0),
  (upid)
)
JOIN process USING (upid)
WHERE count > 2  -- More than 2 concurrent slices
ORDER BY upid, count DESC, dur DESC;
```

**Use case**: Identify processes with high concurrency/parallelism.

## Example 2b: Self-Intersect Per Process with Average Depth

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
  avg AS avg_depth
FROM _interval_self_intersect_avg(
  (SELECT id, ts, dur, depth, upid FROM slice WHERE dur > 0),
  (upid),
  depth
)
JOIN process USING (upid)
WHERE count > 2
ORDER BY upid, avg DESC, count DESC;
```

**Use case**: Identify processes with high concurrency weighted by depth.

## Example 3: Self-Intersect Per Package with Max Depth

Analyze overlaps grouped by Android package:

```sql
INCLUDE PERFETTO MODULE intervals.self_intersect;

-- Get package name for each process
CREATE PERFETTO TABLE slice_with_package AS
SELECT
  slice.id,
  slice.ts,
  slice.dur,
  package_list.package_name,
  slice.depth
FROM slice
JOIN thread USING (utid)
JOIN process USING (upid)
JOIN package_list ON process.uid = package_list.uid
WHERE slice.dur > 0;

-- Self-intersect by package with max depth
SELECT
  package_name,
  ts,
  dur,
  count AS concurrent_slices,
  max AS max_depth
FROM _interval_self_intersect_max(
  slice_with_package,
  (package_name),
  depth
)
WHERE count > 3
ORDER BY package_name, max DESC, count DESC
LIMIT 50;
```

**Use case**: Analyze concurrency patterns per Android app with peak depth.

## Example 4: Self-Intersect Per Thread (utid)

Analyze overlaps per thread:

```sql
INCLUDE PERFETTO MODULE intervals.self_intersect;

-- Analyze overlaps per thread
SELECT
  utid,
  thread.name AS thread_name,
  ts,
  dur,
  count AS overlapping_slices
FROM _interval_self_intersect(
  (SELECT id, ts, dur, utid FROM slice WHERE dur > 0),
  (utid)
)
JOIN thread USING (utid)
WHERE count > 1
ORDER BY utid, count DESC, ts;
```

**Use case**: Find threads with overlapping slices (usually indicates async operations).

**Note**: Multi-level partitioning (e.g., upid AND utid) is not supported in the current implementation. Use a single partition column or create a composite key.

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
  sum AS total_priority
FROM _interval_self_intersect_sum(
  (SELECT id, ts, dur, priority, cpu FROM sched WHERE dur > 0),
  (cpu),
  priority
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
  sum / 1024 / 1024 AS total_mb
FROM _interval_self_intersect_sum(
  (SELECT id, ts, dur, size, upid FROM heap_profile_allocation WHERE dur > 0),
  (upid),
  size
)
JOIN process USING (upid)
WHERE count >= 5  -- High concurrency
ORDER BY total_mb DESC
LIMIT 30;
```

**Use case**: Identify memory pressure hotspots by total allocation size.

## Example 6b: Memory Allocation Pressure - Largest Allocation

```sql
INCLUDE PERFETTO MODULE intervals.self_intersect;

-- Find largest concurrent allocation
SELECT
  upid,
  process.name,
  ts,
  dur,
  count AS concurrent_allocations,
  max / 1024 / 1024 AS largest_mb
FROM _interval_self_intersect_max(
  (SELECT id, ts, dur, size, upid FROM heap_profile_allocation WHERE dur > 0),
  (upid),
  size
)
JOIN process USING (upid)
WHERE count >= 5
ORDER BY largest_mb DESC
LIMIT 30;
```

**Use case**: Identify memory pressure hotspots by largest allocation.

## Example 7: Binder Transaction Overlaps - Count Only

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
FROM _interval_self_intersect(
  (
    SELECT
      id,
      ts,
      dur,
      upid
    FROM slice
    WHERE name GLOB 'binder*' AND dur > 0
  ),
  (upid)
)
JOIN process USING (upid)
WHERE count > 2
ORDER BY count DESC, dur DESC;
```

**Use case**: Identify binder bottlenecks and concurrent IPC patterns.

## Example 8: Frame Rendering Overlaps with Max Vsync ID

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
  max AS max_vsync_id
FROM _interval_self_intersect_max(
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
  vsync_id
)
JOIN process USING (upid)
WHERE count > 1  -- Overlapping frames
ORDER BY count DESC;
```

**Use case**: Detect frame pacing issues and concurrent frame rendering.

## Example 9: Network Request Concurrency - Total Bytes

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
  sum / 1024 AS total_kb
FROM _interval_self_intersect_sum(
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
  bytes
)
JOIN process USING (upid)
WHERE count >= 3
ORDER BY concurrent_requests DESC, total_kb DESC;
```

**Use case**: Understand network concurrency patterns by total bandwidth.

## Example 10: Thread State Self-Intersect - Validation

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
FROM _interval_self_intersect(
  (SELECT id, ts, dur, utid FROM thread_state WHERE dur > 0),
  (utid)
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
FROM _interval_self_intersect(
  (SELECT id, ts, dur, upid FROM slice WHERE dur > 1000000),  -- Only long slices
  (upid)
)

-- BAD: Filter after
FROM _interval_self_intersect(
  (SELECT id, ts, dur, upid FROM slice),
  (upid)
)
WHERE dur > 1000000  -- Filtering after is less efficient
```

### 2. Use Appropriate Partitions

```sql
-- GOOD: Partition by high-cardinality column
FROM _interval_self_intersect(
  slice_data,
  (upid)  -- ~100s of processes
)

-- LESS EFFICIENT: No partition with many intervals
FROM _interval_self_intersect(
  slice_data,
  ()  -- All intervals in one partition
)
```

### 3. Choose the Right Aggregation Function

```sql
-- Use specific functions for what you need:
_interval_self_intersect(...)           -- Count only (fastest)
_interval_self_intersect_sum(..., col)  -- Count + sum
_interval_self_intersect_max(..., col)  -- Count + max
_interval_self_intersect_min(..., col)  -- Count + min
_interval_self_intersect_avg(..., col)  -- Count + avg
```

### 4. Column Order Matters

```sql
-- CORRECT: Aggregation column before partition column
SELECT id, ts, dur, value, upid FROM table

-- For _interval_self_intersect_sum(data, (upid), value)
-- The 'value' column must come before 'upid' in the SELECT
```

## API Summary

### Available Functions

1. **`_interval_self_intersect(table, (partition_col))`**
   - Returns: `ts, dur, group_id, count, partition_col`
   - Use: Count overlapping intervals only

2. **`_interval_self_intersect_sum(table, (partition_col), agg_col)`**
   - Returns: `ts, dur, group_id, count, sum, partition_col`
   - Use: Count + sum of aggregation column

3. **`_interval_self_intersect_max(table, (partition_col), agg_col)`**
   - Returns: `ts, dur, group_id, count, max, partition_col`
   - Use: Count + maximum of aggregation column

4. **`_interval_self_intersect_min(table, (partition_col), agg_col)`**
   - Returns: `ts, dur, group_id, count, min, partition_col`
   - Use: Count + minimum of aggregation column

5. **`_interval_self_intersect_avg(table, (partition_col), agg_col)`**
   - Returns: `ts, dur, group_id, count, avg, partition_col`
   - Use: Count + average of aggregation column

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
| **Performance** | O(n log n) with bitsets | O(n log n) |
| **Aggregations** | Built-in (count, sum, etc.) | Manual via GROUP BY |
| **Partitioning** | 1 column max | Via PARTITIONED keyword |
| **Output** | Buckets with group_id | Intersected intervals |

**When to use Self-Intersect**:
- Analyzing concurrency within a single dataset
- Need aggregations across overlapping intervals
- Want to identify time periods by overlap count
- Working with dense interval IDs (< 100k)

**When to use Span Join**:
- Correlating two different interval sets
- Need to preserve individual interval IDs
- Building complex interval relationships
- Need multi-column partitioning
