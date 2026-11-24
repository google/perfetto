--
-- Copyright 2026 The Android Open Source Project
--
-- Licensed under the Apache License, Version 2.0 (the "License");
-- you may not use this file except in compliance with the License.
-- You may obtain a copy of the License at
--
--     https://www.apache.org/licenses/LICENSE-2.0
--
-- Unless required by applicable law or agreed to in writing, software
-- distributed under the License is distributed on an "AS IS" BASIS,
-- WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
-- See the License for the specific language governing permissions and
-- limitations under the License.

-- Diagnostic stats and parser errors collected at trace time and during
-- import. One row per (key, idx, machine_id, trace_id) combination.
CREATE PERFETTO VIEW stats(
  -- Unique identifier for this stats row.
  id ID,
  -- Stat name (e.g. ftrace_cpu_overrun_begin).
  name STRING,
  -- Numeric stat key: the stats::KeyIDs enum value, or, for a dynamic stat
  -- whose name is only known at runtime, a value from stats::kNumKeys up.
  key LONG,
  -- Per-stat index for indexed and indexed-histogram stats;
  -- NULL for single stats.
  idx LONG,
  -- "info" | "notice" | "data_loss" | "error".
  severity STRING,
  -- "trace" (recorded on-device) or "analysis" (TP).
  source STRING,
  -- Stat value. For indexed-histogram stats this is the total sample count
  -- across all buckets; per-bucket counts are in `stats_histogram_bucket`.
  value LONG,
  -- Human-readable description.
  description STRING,
  -- Machine identifier (NULL for kGlobal stats).
  machine_id JOINID(machine.id),
  -- Trace identifier (NULL for kGlobal stats).
  trace_id LONG
)
AS
SELECT
  id,
  name,
  key,
  idx,
  severity,
  source,
  value,
  description,
  machine_id,
  trace_id
FROM __intrinsic_stats;

-- Buckets of histogram stats. One row per bucket of a histogram stat row in
-- the `stats` table (i.e. per (stat, idx) pair).
CREATE PERFETTO VIEW stats_histogram_bucket(
  -- Unique identifier for this bucket.
  id ID,
  -- The histogram stat this bucket belongs to.
  stat_id JOINID(stats.id),
  -- Inclusive lower bound of the bucket.
  lower_bound LONG,
  -- Exclusive upper bound of the bucket.
  upper_bound LONG,
  -- Number of samples in [lower_bound, upper_bound).
  count LONG
)
AS
SELECT id, stat_id, lower_bound, upper_bound, count
FROM __intrinsic_histogram_bucket;
