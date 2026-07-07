-- NVIDIA compute kernels — workload analysis (instruction throughput & pipes).
--
-- For every compute kernel: how busy the SM was issuing instructions and which
-- execution pipelines carried the work. This is the data for "which pipe is the
-- limit, and is the instruction mix balanced?".
--
-- All values are COMPUTE-group counters (gpu_counter_group.group_id = 6),
-- matched to each kernel's GPU and averaged over its window (these are all
-- rate/percentage metrics -> AVG).
--
-- No parameters; operates on the whole trace. One row per compute kernel,
-- longest first. Columns:
--   id          : launch order (matches kernels_summary.sql).
--   kernel      : demangled kernel name.
--   ipc_active  : instructions executed per active cycle (sm__inst_executed
--                 .avg.per_cycle_active).
--   sm_busy_pct : instruction-issue throughput, % of peak (sm__instruction
--                 _throughput.avg.pct_of_peak_sustained_active).
--   alu/fma/fp16/fp32/fp64/tensor_pct : that pipe's active cycles, % of peak
--                 (sm__pipe_<x>_cycles_active.avg.pct_of_peak_sustained_active).
--
-- Interpret: the highest pipe % is the most-utilized pipeline; a single pipe
-- near peak while sm_busy_pct is lower points to a pipe bottleneck / imbalance;
-- consider a different precision or instruction mix. See compute/workload_analysis.md.

CREATE PERFETTO TABLE _kernels AS
SELECT
  s.id,
  s.ts,
  s.dur,
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
SELECT k.id AS kernel_id, ct.name AS counter_name, AVG(c.value) AS avg_v
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
        WHEN kc.counter_name = 'sm__inst_executed.avg.per_cycle_active' THEN kc.avg_v
      END
    ),
    2
  ) AS ipc_active,
  ROUND(
    MAX(
      CASE
        WHEN kc.counter_name
        = 'sm__instruction_throughput.avg.pct_of_peak_sustained_active' THEN kc.avg_v
      END
    ),
    1
  ) AS sm_busy_pct,
  ROUND(
    MAX(
      CASE
        WHEN kc.counter_name
        = 'sm__pipe_alu_cycles_active.avg.pct_of_peak_sustained_active' THEN kc.avg_v
      END
    ),
    1
  ) AS alu_pct,
  ROUND(
    MAX(
      CASE
        WHEN kc.counter_name
        = 'sm__pipe_fma_cycles_active.avg.pct_of_peak_sustained_active' THEN kc.avg_v
      END
    ),
    1
  ) AS fma_pct,
  ROUND(
    MAX(
      CASE
        WHEN kc.counter_name
        = 'sm__pipe_fp16_cycles_active.avg.pct_of_peak_sustained_active' THEN kc.avg_v
      END
    ),
    1
  ) AS fp16_pct,
  ROUND(
    MAX(
      CASE
        WHEN kc.counter_name
        = 'sm__pipe_fp32_cycles_active.avg.pct_of_peak_sustained_active' THEN kc.avg_v
      END
    ),
    1
  ) AS fp32_pct,
  ROUND(
    MAX(
      CASE
        WHEN kc.counter_name
        = 'sm__pipe_fp64_cycles_active.avg.pct_of_peak_sustained_active' THEN kc.avg_v
      END
    ),
    1
  ) AS fp64_pct,
  ROUND(
    MAX(
      CASE
        WHEN kc.counter_name
        = 'sm__pipe_tensor_cycles_active.avg.pct_of_peak_sustained_active' THEN kc.avg_v
      END
    ),
    1
  ) AS tensor_pct
FROM _kernels AS k
LEFT JOIN _kernel_counters AS kc ON kc.kernel_id = k.id
GROUP BY
  k.launch_id,
  k.kernel,
  k.dur
ORDER BY
  k.dur DESC;
