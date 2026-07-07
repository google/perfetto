-- Compute kernels — launch configuration (vendor-neutral).
--
-- For every compute kernel, the configuration it was launched with: the work
-- size, how it divides into blocks, and the per-block GPU resources. This is
-- the data for "is the launch config efficient?". These are all launch ARGS on
-- the kernel slice and are vendor-neutral (the same keys are emitted by CUDA,
-- HIP and other compute producers), so this script needs no vendor layer.
--
-- No parameters; operates on the whole trace. One row per compute kernel,
-- longest first. Columns:
--   id                 : launch order (matches kernels_summary.sql).
--   kernel             : demangled kernel name.
--   block_size         : threads per block (workgroup_size).
--   grid_size          : number of blocks.
--   thread_count       : total threads (thread_count).
--   registers          : registers per thread.
--   shared_mem_static  : static shared memory per block, bytes.
--   shared_mem_dynamic : dynamic shared memory per block, bytes.
--   shared_mem_config  : shared memory carveout / config size, bytes.
--   barriers_per_block : named barriers per block.
--   waves_per_sm       : waves_per_multiprocessor.
--   func_cache_config  : function cache configuration.
--
-- Interpret: block_size should be a multiple of the warp/wavefront size (32 on
-- NVIDIA, 64 on AMD); very small blocks underuse each SM, very large blocks can
-- cap occupancy via register/shared-memory pressure; waves_per_sm >= 4 balances
-- load. See compute/launch_statistics.md.

CREATE PERFETTO TABLE _kernels AS
SELECT
  s.id,
  s.ts,
  s.dur,
  s.arg_set_id,
  ROW_NUMBER() OVER (ORDER BY s.ts) AS launch_id,
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
  EXTRACT_ARG(arg_set_id, 'workgroup_size') AS block_size,
  EXTRACT_ARG(arg_set_id, 'grid_size') AS grid_size,
  EXTRACT_ARG(arg_set_id, 'thread_count') AS thread_count,
  EXTRACT_ARG(arg_set_id, 'registers_per_thread') AS registers,
  EXTRACT_ARG(arg_set_id, 'shared_mem_static') AS shared_mem_static,
  EXTRACT_ARG(arg_set_id, 'shared_mem_dynamic') AS shared_mem_dynamic,
  EXTRACT_ARG(arg_set_id, 'shared_mem_config_size') AS shared_mem_config,
  EXTRACT_ARG(arg_set_id, 'barriers_per_block') AS barriers_per_block,
  EXTRACT_ARG(arg_set_id, 'waves_per_multiprocessor') AS waves_per_sm,
  EXTRACT_ARG(arg_set_id, 'func_cache_config') AS func_cache_config
FROM _kernels
ORDER BY
  dur DESC;
