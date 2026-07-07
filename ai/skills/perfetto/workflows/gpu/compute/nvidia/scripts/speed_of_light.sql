-- NVIDIA compute kernels — Speed of Light throughput (compute vs memory).
--
-- For every compute kernel, the high-level throughput picture: how close the
-- SM (compute) and the memory system ran to their peaks, plus the cache /
-- DRAM throughput and cycle counts behind it. This is the data for the
-- "compute-bound vs memory-bound vs latency-bound" verdict. The percentages are
-- throughput vs peak, not cache hit rates.
--
-- Counters come from the COMPUTE counter group (gpu_counter_group.group_id = 6)
-- matched to each kernel's GPU and aggregated over the kernel's window.
-- Aggregation per metric: throughput percentages and frequencies use AVG; cycle
-- counts and duration use SUM.
--
-- No parameters; operates on the whole trace. One row per compute kernel,
-- longest first. Columns:
--   id                : launch order (matches kernels_summary.sql).
--   kernel            : demangled kernel name.
--   compute_pct       : sm__throughput, % of peak (AVG). The compute ceiling.
--   memory_pct        : gpu__compute_memory_throughput, % of peak (AVG). The
--                       memory-system ceiling.
--   l1_pct, l2_pct    : l1tex / lts throughput, % of peak (AVG).
--   dram_pct          : gpu__dram_throughput, % of peak (AVG).
--   elapsed_cycles    : gpc__cycles_elapsed.max (SUM).
--   active_cycles     : sm__cycles_active.avg (SUM).
--   dur_ns            : gpu__time_duration.sum (SUM); slice dur if absent.
--
-- Interpret: high compute_pct & lower memory_pct -> compute-bound; high
-- memory_pct -> memory-bound (use l1/l2/dram_pct to locate it); both low ->
-- latency/occupancy-bound; active_cycles << elapsed_cycles corroborates a
-- latency/occupancy stall. See compute/speed_of_light.md.

CREATE PERFETTO TABLE _kernels AS
SELECT
  s.id,
  s.ts,
  s.dur,
  s.arg_set_id,
  ROW_NUMBER() OVER (ORDER BY s.ts) AS launch_id,
  EXTRACT_ARG(t.dimension_arg_set_id, 'ugpu') AS ugpu,
  COALESCE(
    EXTRACT_ARG(s.arg_set_id, 'kernel_demangled_name'),
    EXTRACT_ARG(s.arg_set_id, 'kernel_name'),
    s.name
  ) AS kernel
FROM gpu_slice AS s
JOIN gpu_track AS t
  ON s.track_id = t.id
WHERE
  s.render_stage_category = 2
  AND s.dur > 0;

CREATE PERFETTO TABLE _kernel_counters AS
SELECT
  k.id AS kernel_id,
  ct.name AS counter_name,
  SUM(c.value) AS sum_v,
  AVG(c.value) AS avg_v
FROM _kernels AS k
JOIN gpu_counter_group AS g ON g.group_id = 6
JOIN gpu_counter_track AS ct ON ct.id = g.track_id AND ct.ugpu = k.ugpu
JOIN counter AS c ON c.track_id = ct.id AND c.ts >= k.ts AND c.ts < k.ts + k.dur
GROUP BY
  k.id,
  ct.name;

SELECT
  k.launch_id AS id,
  k.kernel,
  ROUND(
    MAX(
      CASE
        WHEN kc.counter_name
        = 'sm__throughput.avg.pct_of_peak_sustained_elapsed' THEN kc.avg_v
      END
    ),
    1
  ) AS compute_pct,
  ROUND(
    MAX(
      CASE
        WHEN kc.counter_name
        = 'gpu__compute_memory_throughput.avg.pct_of_peak_sustained_elapsed' THEN kc.avg_v
      END
    ),
    1
  ) AS memory_pct,
  ROUND(
    MAX(
      CASE
        WHEN kc.counter_name
        = 'l1tex__throughput.avg.pct_of_peak_sustained_active' THEN kc.avg_v
      END
    ),
    1
  ) AS l1_pct,
  ROUND(
    MAX(
      CASE
        WHEN kc.counter_name
        = 'lts__throughput.avg.pct_of_peak_sustained_elapsed' THEN kc.avg_v
      END
    ),
    1
  ) AS l2_pct,
  ROUND(
    MAX(
      CASE
        WHEN kc.counter_name
        = 'gpu__dram_throughput.avg.pct_of_peak_sustained_elapsed' THEN kc.avg_v
      END
    ),
    1
  ) AS dram_pct,
  CAST(MAX(
    CASE WHEN kc.counter_name = 'gpc__cycles_elapsed.max' THEN kc.sum_v END
  ) AS INT) AS elapsed_cycles,
  CAST(MAX(
    CASE WHEN kc.counter_name = 'sm__cycles_active.avg' THEN kc.sum_v END
  ) AS INT) AS active_cycles,
  CAST(IFNULL(
    MAX(CASE WHEN kc.counter_name = 'gpu__time_duration.sum' THEN kc.sum_v END),
    k.dur
  ) AS INT) AS dur_ns
FROM _kernels AS k
LEFT JOIN _kernel_counters AS kc ON kc.kernel_id = k.id
GROUP BY
  k.launch_id,
  k.kernel,
  k.dur
ORDER BY
  k.dur DESC;
