-- NVIDIA compute kernels — occupancy and its limiting factor.
--
-- For every compute kernel: theoretical vs achieved occupancy, the launch
-- resources that bound it, and which resource is the binding constraint. This
-- is the data for "is occupancy limiting this kernel, and what caps it?".
--
-- Theoretical occupancy and the per-resource block limits are recorded as
-- launch ARGS on the kernel slice (sm__maximum_warps_*, occupancy_limit_*);
-- achieved occupancy is a COMPUTE-group COUNTER (sm__warps_active.*, AVG),
-- matched to the kernel's GPU over its window. The binding resource is the
-- occupancy_limit_* with the smallest value (fewest blocks/SM allowed);
-- limits that are absent are skipped, and ties resolve in the order
-- blocks, registers, shared_mem, warps, barriers.
--
-- No parameters; operates on the whole trace. One row per compute kernel,
-- longest first. Columns:
--   id                  : launch order (matches kernels_summary.sql).
--   kernel              : demangled kernel name.
--   theoretical_occ_pct : sm__maximum_warps_per_active_cycle_pct (arg).
--   achieved_occ_pct    : sm__warps_active.avg.pct_of_peak_sustained_active (AVG).
--   block_size          : threads per block (workgroup_size).
--   grid_size           : number of blocks.
--   registers           : registers per thread.
--   shared_mem_static   : static shared memory per block, bytes.
--   shared_mem_dynamic  : dynamic shared memory per block, bytes.
--   waves_per_sm        : waves_per_multiprocessor (grid / SM-resident blocks).
--   limit_blocks/registers/shared_mem/warps/barriers : blocks/SM allowed by
--                         each resource (occupancy_limit_*).
--   binding_resource    : the resource at the smallest limit (the bottleneck);
--                         NULL only if no limit arg is present at all.
--
-- Interpret: block_size should be a multiple of the warp size (32 on NVIDIA);
-- waves_per_sm < 1 = grid too small to fill the GPU, 1-2 = tail effects, >= 4 =
-- good load balance. A large theoretical-vs-achieved gap means imbalance/tails;
-- reduce the binding_resource to raise theoretical occupancy if occupancy is
-- the limit. See compute/occupancy.md.

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

CREATE PERFETTO TABLE _achieved AS
SELECT k.id AS kernel_id, AVG(c.value) AS achieved_occ_pct
FROM _kernels AS k
JOIN gpu_counter_group AS g ON g.group_id = 6
JOIN gpu_counter_track AS ct
  ON ct.id = g.track_id
  AND ct.ugpu = k.ugpu
  AND ct.name = 'sm__warps_active.avg.pct_of_peak_sustained_active'
JOIN counter AS c ON c.track_id = ct.id AND c.ts >= k.ts AND c.ts < k.ts + k.dur
GROUP BY
  k.id;

CREATE PERFETTO TABLE _occ AS
SELECT
  k.id AS kernel_id,
  k.launch_id AS id,
  k.kernel,
  k.dur,
  CAST(EXTRACT_ARG(k.arg_set_id, 'sm__maximum_warps_per_active_cycle_pct') AS REAL) AS theoretical_occ_pct,
  ROUND(a.achieved_occ_pct, 1) AS achieved_occ_pct,
  EXTRACT_ARG(k.arg_set_id, 'workgroup_size') AS block_size,
  EXTRACT_ARG(k.arg_set_id, 'grid_size') AS grid_size,
  EXTRACT_ARG(k.arg_set_id, 'registers_per_thread') AS registers,
  EXTRACT_ARG(k.arg_set_id, 'shared_mem_static') AS shared_mem_static,
  EXTRACT_ARG(k.arg_set_id, 'shared_mem_dynamic') AS shared_mem_dynamic,
  EXTRACT_ARG(k.arg_set_id, 'waves_per_multiprocessor') AS waves_per_sm,
  EXTRACT_ARG(k.arg_set_id, 'occupancy_limit_blocks') AS limit_blocks,
  EXTRACT_ARG(k.arg_set_id, 'occupancy_limit_registers') AS limit_registers,
  EXTRACT_ARG(k.arg_set_id, 'occupancy_limit_shared_mem') AS limit_shared_mem,
  EXTRACT_ARG(k.arg_set_id, 'occupancy_limit_warps') AS limit_warps,
  EXTRACT_ARG(k.arg_set_id, 'occupancy_limit_barriers') AS limit_barriers
FROM _kernels AS k
LEFT JOIN _achieved AS a ON a.kernel_id = k.id;

-- Unpivot the per-resource block limits to one row each, so the binding
-- resource is the smallest PRESENT limit (NULLs skipped) with a deterministic
-- tie-break. SQLite's scalar MIN(a,b,...) would instead return NULL whenever any
-- single limit arg is absent, hiding an otherwise-unambiguous bottleneck.
CREATE PERFETTO TABLE _limits AS
SELECT kernel_id, 1 AS rank, 'blocks' AS resource, limit_blocks AS v FROM _occ
UNION ALL
SELECT kernel_id, 2, 'registers', limit_registers FROM _occ
UNION ALL
SELECT kernel_id, 3, 'shared_mem', limit_shared_mem FROM _occ
UNION ALL
SELECT kernel_id, 4, 'warps', limit_warps FROM _occ
UNION ALL
SELECT kernel_id, 5, 'barriers', limit_barriers FROM _occ;

CREATE PERFETTO TABLE _binding AS
SELECT kernel_id, resource AS binding_resource
FROM (
  SELECT
    kernel_id,
    resource,
    ROW_NUMBER() OVER (PARTITION BY kernel_id ORDER BY v, rank) AS rn
  FROM _limits
  WHERE
    v IS NOT NULL
)
WHERE
  rn = 1;

SELECT
  o.id,
  o.kernel,
  o.theoretical_occ_pct,
  o.achieved_occ_pct,
  o.block_size,
  o.grid_size,
  o.registers,
  o.shared_mem_static,
  o.shared_mem_dynamic,
  o.waves_per_sm,
  o.limit_blocks,
  o.limit_registers,
  o.limit_shared_mem,
  o.limit_warps,
  o.limit_barriers,
  b.binding_resource
FROM _occ AS o
LEFT JOIN _binding AS b ON b.kernel_id = o.kernel_id
ORDER BY
  o.dur DESC;
