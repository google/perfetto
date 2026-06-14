--
-- Copyright 2024 The Android Open Source Project
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

INCLUDE PERFETTO MODULE intervals.intersect;

INCLUDE PERFETTO MODULE wattson.cpu.arm_dsu;

INCLUDE PERFETTO MODULE wattson.cpu.freq_idle;

INCLUDE PERFETTO MODULE wattson.cpu.hotplug;

INCLUDE PERFETTO MODULE wattson.curves.utils;

INCLUDE PERFETTO MODULE wattson.device_infos;

INCLUDE PERFETTO MODULE wattson.utils;

-- The _cpu_stats_subquery! relational macro is inlined into each _stats_cpuN
-- pipeline below (with its scalar cpu/column-name arguments substituted).

CREATE PERFETTO PIPELINE _stats_cpu0 MATERIALIZED AS
SUBPIPELINE _fallback AS (
  FROM _deepest_idle()
  |> WHERE NOT EXISTS(
       SELECT 1 FROM _dev_cpu_policy_map WHERE cpu = 0
     )
  |> SELECT
       trace_start() AS ts,
       trace_dur() AS dur,
       0 AS cpu0_curve,
       0 AS cpu0_static,
       0 AS freq_0,
       idle AS idle_0
)
FROM _idle_freq_materialized AS t1
|> CROSS JOIN _deepest_idle AS deepest
|> WHERE cpu = 0
|> SELECT
     t1.ts,
     t1.dur,
     t1.curve_value AS cpu0_curve,
     iif(0 IN _device_policies, coalesce(t1.static, 0), 0) AS cpu0_static,
     coalesce(t1.freq, 0) AS freq_0,
     coalesce(t1.idle, deepest.idle) AS idle_0
|> UNION ALL (FROM _fallback);

CREATE PERFETTO PIPELINE _stats_cpu1 MATERIALIZED AS
SUBPIPELINE _fallback AS (
  FROM _deepest_idle()
  |> WHERE NOT EXISTS(
       SELECT 1 FROM _dev_cpu_policy_map WHERE cpu = 1
     )
  |> SELECT
       trace_start() AS ts,
       trace_dur() AS dur,
       0 AS cpu1_curve,
       0 AS cpu1_static,
       0 AS freq_1,
       idle AS idle_1
)
FROM _idle_freq_materialized AS t1
|> CROSS JOIN _deepest_idle AS deepest
|> WHERE cpu = 1
|> SELECT
     t1.ts,
     t1.dur,
     t1.curve_value AS cpu1_curve,
     iif(1 IN _device_policies, coalesce(t1.static, 0), 0) AS cpu1_static,
     coalesce(t1.freq, 0) AS freq_1,
     coalesce(t1.idle, deepest.idle) AS idle_1
|> UNION ALL (FROM _fallback);

CREATE PERFETTO PIPELINE _stats_cpu2 MATERIALIZED AS
SUBPIPELINE _fallback AS (
  FROM _deepest_idle()
  |> WHERE NOT EXISTS(
       SELECT 1 FROM _dev_cpu_policy_map WHERE cpu = 2
     )
  |> SELECT
       trace_start() AS ts,
       trace_dur() AS dur,
       0 AS cpu2_curve,
       0 AS cpu2_static,
       0 AS freq_2,
       idle AS idle_2
)
FROM _idle_freq_materialized AS t1
|> CROSS JOIN _deepest_idle AS deepest
|> WHERE cpu = 2
|> SELECT
     t1.ts,
     t1.dur,
     t1.curve_value AS cpu2_curve,
     iif(2 IN _device_policies, coalesce(t1.static, 0), 0) AS cpu2_static,
     coalesce(t1.freq, 0) AS freq_2,
     coalesce(t1.idle, deepest.idle) AS idle_2
|> UNION ALL (FROM _fallback);

CREATE PERFETTO PIPELINE _stats_cpu3 MATERIALIZED AS
SUBPIPELINE _fallback AS (
  FROM _deepest_idle()
  |> WHERE NOT EXISTS(
       SELECT 1 FROM _dev_cpu_policy_map WHERE cpu = 3
     )
  |> SELECT
       trace_start() AS ts,
       trace_dur() AS dur,
       0 AS cpu3_curve,
       0 AS cpu3_static,
       0 AS freq_3,
       idle AS idle_3
)
FROM _idle_freq_materialized AS t1
|> CROSS JOIN _deepest_idle AS deepest
|> WHERE cpu = 3
|> SELECT
     t1.ts,
     t1.dur,
     t1.curve_value AS cpu3_curve,
     iif(3 IN _device_policies, coalesce(t1.static, 0), 0) AS cpu3_static,
     coalesce(t1.freq, 0) AS freq_3,
     coalesce(t1.idle, deepest.idle) AS idle_3
|> UNION ALL (FROM _fallback);

CREATE PERFETTO PIPELINE _stats_cpu4 MATERIALIZED AS
SUBPIPELINE _fallback AS (
  FROM _deepest_idle()
  |> WHERE NOT EXISTS(
       SELECT 1 FROM _dev_cpu_policy_map WHERE cpu = 4
     )
  |> SELECT
       trace_start() AS ts,
       trace_dur() AS dur,
       0 AS cpu4_curve,
       0 AS cpu4_static,
       0 AS freq_4,
       idle AS idle_4
)
FROM _idle_freq_materialized AS t1
|> CROSS JOIN _deepest_idle AS deepest
|> WHERE cpu = 4
|> SELECT
     t1.ts,
     t1.dur,
     t1.curve_value AS cpu4_curve,
     iif(4 IN _device_policies, coalesce(t1.static, 0), 0) AS cpu4_static,
     coalesce(t1.freq, 0) AS freq_4,
     coalesce(t1.idle, deepest.idle) AS idle_4
|> UNION ALL (FROM _fallback);

CREATE PERFETTO PIPELINE _stats_cpu5 MATERIALIZED AS
SUBPIPELINE _fallback AS (
  FROM _deepest_idle()
  |> WHERE NOT EXISTS(
       SELECT 1 FROM _dev_cpu_policy_map WHERE cpu = 5
     )
  |> SELECT
       trace_start() AS ts,
       trace_dur() AS dur,
       0 AS cpu5_curve,
       0 AS cpu5_static,
       0 AS freq_5,
       idle AS idle_5
)
FROM _idle_freq_materialized AS t1
|> CROSS JOIN _deepest_idle AS deepest
|> WHERE cpu = 5
|> SELECT
     t1.ts,
     t1.dur,
     t1.curve_value AS cpu5_curve,
     iif(5 IN _device_policies, coalesce(t1.static, 0), 0) AS cpu5_static,
     coalesce(t1.freq, 0) AS freq_5,
     coalesce(t1.idle, deepest.idle) AS idle_5
|> UNION ALL (FROM _fallback);

CREATE PERFETTO PIPELINE _stats_cpu6 MATERIALIZED AS
SUBPIPELINE _fallback AS (
  FROM _deepest_idle()
  |> WHERE NOT EXISTS(
       SELECT 1 FROM _dev_cpu_policy_map WHERE cpu = 6
     )
  |> SELECT
       trace_start() AS ts,
       trace_dur() AS dur,
       0 AS cpu6_curve,
       0 AS cpu6_static,
       0 AS freq_6,
       idle AS idle_6
)
FROM _idle_freq_materialized AS t1
|> CROSS JOIN _deepest_idle AS deepest
|> WHERE cpu = 6
|> SELECT
     t1.ts,
     t1.dur,
     t1.curve_value AS cpu6_curve,
     iif(6 IN _device_policies, coalesce(t1.static, 0), 0) AS cpu6_static,
     coalesce(t1.freq, 0) AS freq_6,
     coalesce(t1.idle, deepest.idle) AS idle_6
|> UNION ALL (FROM _fallback);

CREATE PERFETTO PIPELINE _stats_cpu7 MATERIALIZED AS
SUBPIPELINE _fallback AS (
  FROM _deepest_idle()
  |> WHERE NOT EXISTS(
       SELECT 1 FROM _dev_cpu_policy_map WHERE cpu = 7
     )
  |> SELECT
       trace_start() AS ts,
       trace_dur() AS dur,
       0 AS cpu7_curve,
       0 AS cpu7_static,
       0 AS freq_7,
       idle AS idle_7
)
FROM _idle_freq_materialized AS t1
|> CROSS JOIN _deepest_idle AS deepest
|> WHERE cpu = 7
|> SELECT
     t1.ts,
     t1.dur,
     t1.curve_value AS cpu7_curve,
     iif(7 IN _device_policies, coalesce(t1.static, 0), 0) AS cpu7_static,
     coalesce(t1.freq, 0) AS freq_7,
     coalesce(t1.idle, deepest.idle) AS idle_7
|> UNION ALL (FROM _fallback);

CREATE PERFETTO PIPELINE _all_stats MATERIALIZED AS
-- The positional _interval_intersect! over 11 operands becomes an
-- INTERVAL INTERSECTION OF with nominal column refs; the JOIN ... ON
-- _auto_id = id_N rewiring is subsumed by the co-fragmentation.
INTERVAL INTERSECTION OF (
  _stats_cpu0 AS c0,
  _stats_cpu1 AS c1,
  _stats_cpu2 AS c2,
  _stats_cpu3 AS c3,
  _stats_cpu4 AS c4,
  _stats_cpu5 AS c5,
  _stats_cpu6 AS c6,
  _stats_cpu7 AS c7,
  _wattson_dsu_frequency AS dsu,
  _arm_l3_rates AS l3,
  _gapless_suspend_slices AS suspend
) PER ()
|> CROSS JOIN _deepest_idle AS deepest
|> SELECT
     ts,
     dur,
     cast_int!(l3.l3_hit_rate * dur) AS l3_hit_count,
     cast_int!(l3.l3_miss_rate * dur) AS l3_miss_count,
     c0.freq_0,
     c0.idle_0,
     c1.freq_1,
     c1.idle_1,
     c2.freq_2,
     c2.idle_2,
     c3.freq_3,
     c3.idle_3,
     c4.freq_4,
     c4.idle_4,
     c5.freq_5,
     c5.idle_5,
     c6.freq_6,
     c6.idle_6,
     c7.freq_7,
     c7.idle_7,
     c0.cpu0_curve,
     c1.cpu1_curve,
     c2.cpu2_curve,
     c3.cpu3_curve,
     c4.cpu4_curve,
     c5.cpu5_curve,
     c6.cpu6_curve,
     c7.cpu7_curve,
     c0.cpu0_static,
     c1.cpu1_static,
     c2.cpu2_static,
     c3.cpu3_static,
     c4.cpu4_static,
     c5.cpu5_static,
     c6.cpu6_static,
     c7.cpu7_static,
     suspend.suspended,
     dsu.dsu_freq,
     CAST(_bitmask8!(
       c0.idle_0 != deepest.idle,
       c1.idle_1 != deepest.idle,
       c2.idle_2 != deepest.idle,
       c3.idle_3 != deepest.idle,
       c4.idle_4 != deepest.idle,
       c5.idle_5 != deepest.idle,
       c6.idle_6 != deepest.idle,
       c7.idle_7 != deepest.idle
     ) AS INTEGER) AS cpus_on_mask;

-- Does calculations for CPUs that are independent of other CPUs or frequencies
-- This is the last generic table before going to device specific table calcs
CREATE PERFETTO PIPELINE _w_independent_cpus_calc MATERIALIZED AS
FROM _all_stats
|> CROSS JOIN _policy_masks
|> SELECT
     ts,
     dur,
     l3_hit_count,
     l3_miss_count,
     hash(
       freq_0,
       idle_0,
       freq_1,
       idle_1,
       freq_2,
       idle_2,
       freq_3,
       idle_3,
       freq_4,
       idle_4,
       freq_5,
       idle_5,
       freq_6,
       idle_6,
       freq_7,
       idle_7,
       dsu_freq,
       suspended
     ) AS config_hash,
     freq_0,
     idle_0,
     freq_1,
     idle_1,
     freq_2,
     idle_2,
     freq_3,
     idle_3,
     freq_4,
     idle_4,
     freq_5,
     idle_5,
     freq_6,
     idle_6,
     freq_7,
     idle_7,
     cpu0_curve,
     cpu1_curve,
     cpu2_curve,
     cpu3_curve,
     cpu4_curve,
     cpu5_curve,
     cpu6_curve,
     cpu7_curve,
     suspended,
     dsu_freq,
     CAST(_bitmask8!(
       cpus_on_mask & m0,
       cpus_on_mask & m1,
       cpus_on_mask & m2,
       cpus_on_mask & m3,
       cpus_on_mask & m4,
       cpus_on_mask & m5,
       cpus_on_mask & m6,
       cpus_on_mask & m7
     ) AS INTEGER) AS policy_cpus_on_mask,
     iif(cpus_on_mask & m0, cpu0_static, 0)
     + iif(cpus_on_mask & m1, cpu1_static, 0)
     + iif(cpus_on_mask & m2, cpu2_static, 0)
     + iif(cpus_on_mask & m3, cpu3_static, 0)
     + iif(cpus_on_mask & m4, cpu4_static, 0)
     + iif(cpus_on_mask & m5, cpu5_static, 0)
     + iif(cpus_on_mask & m6, cpu6_static, 0)
     + iif(cpus_on_mask & m7, cpu7_static, 0) AS static_1d;

-- Slices view with all UNIQUE configs of independent and dependent CPU data
CREATE PERFETTO PIPELINE _w_dependent_cpus_unique AS
-- Gets DSU dependent CPU upfront as a single row, which means this can be
-- efficiently CROSS JOIN-ed later
SUBPIPELINE dsu_flags AS (
  FROM _cpu_w_dsu_dependency
  |> AGGREGATE
       max(cpu = 0) AS dsu_0,
       max(cpu = 1) AS dsu_1,
       max(cpu = 2) AS dsu_2,
       max(cpu = 3) AS dsu_3,
       max(cpu = 4) AS dsu_4,
       max(cpu = 5) AS dsu_5,
       max(cpu = 6) AS dsu_6,
       max(cpu = 7) AS dsu_7
)
SUBPIPELINE _w_unique_configs AS (
  FROM _w_independent_cpus_calc
  |> AGGREGATE
       ANY_VALUE(freq_0) AS freq_0,
       ANY_VALUE(idle_0) AS idle_0,
       ANY_VALUE(freq_1) AS freq_1,
       ANY_VALUE(idle_1) AS idle_1,
       ANY_VALUE(freq_2) AS freq_2,
       ANY_VALUE(idle_2) AS idle_2,
       ANY_VALUE(freq_3) AS freq_3,
       ANY_VALUE(idle_3) AS idle_3,
       ANY_VALUE(freq_4) AS freq_4,
       ANY_VALUE(idle_4) AS idle_4,
       ANY_VALUE(freq_5) AS freq_5,
       ANY_VALUE(idle_5) AS idle_5,
       ANY_VALUE(freq_6) AS freq_6,
       ANY_VALUE(idle_6) AS idle_6,
       ANY_VALUE(freq_7) AS freq_7,
       ANY_VALUE(idle_7) AS idle_7,
       ANY_VALUE(cpu0_curve) AS cpu0_curve,
       ANY_VALUE(cpu1_curve) AS cpu1_curve,
       ANY_VALUE(cpu2_curve) AS cpu2_curve,
       ANY_VALUE(cpu3_curve) AS cpu3_curve,
       ANY_VALUE(cpu4_curve) AS cpu4_curve,
       ANY_VALUE(cpu5_curve) AS cpu5_curve,
       ANY_VALUE(cpu6_curve) AS cpu6_curve,
       ANY_VALUE(cpu7_curve) AS cpu7_curve,
       ANY_VALUE(dsu_freq) AS dsu_freq,
       ANY_VALUE(static_1d) AS static_1d,
       ANY_VALUE(policy_cpus_on_mask) AS policy_cpus_on_mask,
       ANY_VALUE(suspended) AS suspended
     GROUP BY config_hash
)
-- Only unpivot the necessary columns for dependency calculation.
-- Additionally, only unpivot the necessary rows for dependency calculation
-- based off of _cpu_lut_dependencies. The superset of the CROSS JOIN will be
-- CPU (x0, y0), ..., (x0, yN), ..., (xN, yN). The _cpu_lut_dependencies will
-- eliminate any possible CPU-pairing that are not possible dependencies.
-- Pivot the results back into new columns.
SUBPIPELINE pivoted_results AS (
  FROM _w_unique_configs AS i
  |> CROSS JOIN _cpu_lut_dependencies AS d
  |> JOIN _dev_vote_by_freq AS v
     ON d.cpu = v.cpu
  |> JOIN _dev_cpu_policy_map AS p
     ON d.dep_cpu = p.cpu
  |> WHERE
     CASE d.dep_cpu
       WHEN 0 THEN i.idle_0
       WHEN 1 THEN i.idle_1
       WHEN 2 THEN i.idle_2
       WHEN 3 THEN i.idle_3
       WHEN 4 THEN i.idle_4
       WHEN 5 THEN i.idle_5
       WHEN 6 THEN i.idle_6
       WHEN 7 THEN i.idle_7
     END
     = -1
  |> SELECT
     i.config_hash,
     d.cpu,
     -- Determine the scoring value (Frequency or Curve) based on device
     CASE v.vote_by_freq
       WHEN 1 THEN CASE d.dep_cpu
         WHEN 0 THEN i.freq_0
         WHEN 1 THEN i.freq_1
         WHEN 2 THEN i.freq_2
         WHEN 3 THEN i.freq_3
         WHEN 4 THEN i.freq_4
         WHEN 5 THEN i.freq_5
         WHEN 6 THEN i.freq_6
         WHEN 7 THEN i.freq_7
       END
       ELSE CASE d.dep_cpu
         WHEN 0 THEN i.cpu0_curve
         WHEN 1 THEN i.cpu1_curve
         WHEN 2 THEN i.cpu2_curve
         WHEN 3 THEN i.cpu3_curve
         WHEN 4 THEN i.cpu4_curve
         WHEN 5 THEN i.cpu5_curve
         WHEN 6 THEN i.cpu6_curve
         WHEN 7 THEN i.cpu7_curve
       END
     END AS vote_score,
     -- Calculate the Actual Frequency (to be used in the result)
     CASE d.dep_cpu
       WHEN 0 THEN i.freq_0
       WHEN 1 THEN i.freq_1
       WHEN 2 THEN i.freq_2
       WHEN 3 THEN i.freq_3
       WHEN 4 THEN i.freq_4
       WHEN 5 THEN i.freq_5
       WHEN 6 THEN i.freq_6
       WHEN 7 THEN i.freq_7
     END AS freq,
     p.policy
  |> AGGREGATE ARG_MAX(vote_score, freq) AS freq, ARG_MAX(vote_score, policy) AS policy, max(vote_score)
     GROUP BY config_hash, cpu
  |> AGGREGATE
       max(CASE WHEN cpu = 0 THEN freq END) AS dep_freq_0,
       max(CASE WHEN cpu = 0 THEN policy END) AS dep_policy_0,
       max(CASE WHEN cpu = 1 THEN freq END) AS dep_freq_1,
       max(CASE WHEN cpu = 1 THEN policy END) AS dep_policy_1,
       max(CASE WHEN cpu = 2 THEN freq END) AS dep_freq_2,
       max(CASE WHEN cpu = 2 THEN policy END) AS dep_policy_2,
       max(CASE WHEN cpu = 3 THEN freq END) AS dep_freq_3,
       max(CASE WHEN cpu = 3 THEN policy END) AS dep_policy_3,
       max(CASE WHEN cpu = 4 THEN freq END) AS dep_freq_4,
       max(CASE WHEN cpu = 4 THEN policy END) AS dep_policy_4,
       max(CASE WHEN cpu = 5 THEN freq END) AS dep_freq_5,
       max(CASE WHEN cpu = 5 THEN policy END) AS dep_policy_5,
       max(CASE WHEN cpu = 6 THEN freq END) AS dep_freq_6,
       max(CASE WHEN cpu = 6 THEN policy END) AS dep_policy_6,
       max(CASE WHEN cpu = 7 THEN freq END) AS dep_freq_7,
       max(CASE WHEN cpu = 7 THEN policy END) AS dep_policy_7
     GROUP BY config_hash
)
SUBPIPELINE default_votes AS (
  FROM _cpu_w_dependency_default_vote
  |> AGGREGATE
       max(iif(cpu = 0, dep_policy, NULL)) AS default_dep_policy_0,
       max(iif(cpu = 0, dep_freq, NULL)) AS default_dep_freq_0,
       max(iif(cpu = 1, dep_policy, NULL)) AS default_dep_policy_1,
       max(iif(cpu = 1, dep_freq, NULL)) AS default_dep_freq_1,
       max(iif(cpu = 2, dep_policy, NULL)) AS default_dep_policy_2,
       max(iif(cpu = 2, dep_freq, NULL)) AS default_dep_freq_2,
       max(iif(cpu = 3, dep_policy, NULL)) AS default_dep_policy_3,
       max(iif(cpu = 3, dep_freq, NULL)) AS default_dep_freq_3,
       max(iif(cpu = 4, dep_policy, NULL)) AS default_dep_policy_4,
       max(iif(cpu = 4, dep_freq, NULL)) AS default_dep_freq_4,
       max(iif(cpu = 5, dep_policy, NULL)) AS default_dep_policy_5,
       max(iif(cpu = 5, dep_freq, NULL)) AS default_dep_freq_5,
       max(iif(cpu = 6, dep_policy, NULL)) AS default_dep_policy_6,
       max(iif(cpu = 6, dep_freq, NULL)) AS default_dep_freq_6,
       max(iif(cpu = 7, dep_policy, NULL)) AS default_dep_policy_7,
       max(iif(cpu = 7, dep_freq, NULL)) AS default_dep_freq_7
)
-- Join the calculated dependencies back to the original data.
FROM _w_unique_configs AS base
|> CROSS JOIN dsu_flags AS dsu
|> CROSS JOIN default_votes AS defaults
|> LEFT JOIN pivoted_results AS pivoted USING (config_hash)
|> SELECT
   base.*,
   iif(dsu.dsu_0, dsu_freq, coalesce(dep_freq_0, defaults.default_dep_freq_0)) AS dep_freq_0,
   iif(dsu.dsu_0, 255, coalesce(dep_policy_0, defaults.default_dep_policy_0)) AS dep_policy_0,
   iif(dsu.dsu_1, dsu_freq, coalesce(dep_freq_1, defaults.default_dep_freq_1)) AS dep_freq_1,
   iif(dsu.dsu_1, 255, coalesce(dep_policy_1, defaults.default_dep_policy_1)) AS dep_policy_1,
   iif(dsu.dsu_2, dsu_freq, coalesce(dep_freq_2, defaults.default_dep_freq_2)) AS dep_freq_2,
   iif(dsu.dsu_2, 255, coalesce(dep_policy_2, defaults.default_dep_policy_2)) AS dep_policy_2,
   iif(dsu.dsu_3, dsu_freq, coalesce(dep_freq_3, defaults.default_dep_freq_3)) AS dep_freq_3,
   iif(dsu.dsu_3, 255, coalesce(dep_policy_3, defaults.default_dep_policy_3)) AS dep_policy_3,
   iif(dsu.dsu_4, dsu_freq, coalesce(dep_freq_4, defaults.default_dep_freq_4)) AS dep_freq_4,
   iif(dsu.dsu_4, 255, coalesce(dep_policy_4, defaults.default_dep_policy_4)) AS dep_policy_4,
   iif(dsu.dsu_5, dsu_freq, coalesce(dep_freq_5, defaults.default_dep_freq_5)) AS dep_freq_5,
   iif(dsu.dsu_5, 255, coalesce(dep_policy_5, defaults.default_dep_policy_5)) AS dep_policy_5,
   iif(dsu.dsu_6, dsu_freq, coalesce(dep_freq_6, defaults.default_dep_freq_6)) AS dep_freq_6,
   iif(dsu.dsu_6, 255, coalesce(dep_policy_6, defaults.default_dep_policy_6)) AS dep_policy_6,
   iif(dsu.dsu_7, dsu_freq, coalesce(dep_freq_7, defaults.default_dep_freq_7)) AS dep_freq_7,
   iif(dsu.dsu_7, 255, coalesce(dep_policy_7, defaults.default_dep_policy_7)) AS dep_policy_7;
