-- GPU compute-kernel summary — one row per compute kernel.
--
-- The cross-kernel triage: every compute kernel in the trace with its duration
-- and launch shape, so you can pick the kernel that matters. This is the
-- vendor-neutral first pass — it uses only data available regardless of GPU
-- vendor (slice timing and launch args). The compute-vs-memory bound-type
-- split needs vendor throughput counters and lives in the Speed of Light step.
--
-- A compute kernel is a GPU render-stage slice with render_stage_category = 2
-- (0=OTHER, 1=GRAPHICS, 2=COMPUTE). Only kernels with dur > 0 are listed (a
-- zero/NULL-duration kernel has no time window to analyse); the same filter is
-- used by every compute script so the launch-order id lines up across them.
--
-- No parameters; operates on the whole trace. One row per compute kernel,
-- longest first. Columns:
--   id          : launch order (1 = first-launched kernel), stable within a run.
--   kernel      : demangled kernel name (falls back to mangled / slice name).
--   ugpu        : host-unique GPU id the kernel ran on (see gpu_info.md).
--   dur_ns      : kernel duration.
--   block_size  : threads per block (launch arg).
--   grid_size   : number of blocks (launch arg).
--   registers   : registers per thread (launch arg).
--
-- Interpret: the kernel(s) dominating dur_ns are the hotspots to drill. To find
-- whether a kernel is compute- or memory-bound, run the Speed of Light step
-- (vendor-specific counters). Block/grid/registers are the launch shape; see
-- launch_statistics.md for the full configuration.

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

SELECT
  launch_id AS id,
  kernel,
  ugpu,
  dur AS dur_ns,
  EXTRACT_ARG(arg_set_id, 'workgroup_size') AS block_size,
  EXTRACT_ARG(arg_set_id, 'grid_size') AS grid_size,
  EXTRACT_ARG(arg_set_id, 'registers_per_thread') AS registers
FROM _kernels
ORDER BY
  dur DESC;
