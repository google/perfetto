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

INCLUDE PERFETTO MODULE wattson.cpu.freq;

INCLUDE PERFETTO MODULE wattson.cpu.freq_idle;

INCLUDE PERFETTO MODULE wattson.cpu.hotplug;

INCLUDE PERFETTO MODULE wattson.curves.utils;

INCLUDE PERFETTO MODULE wattson.device_infos;

INCLUDE PERFETTO MODULE wattson.utils;

CREATE PERFETTO TABLE _stats_cpu0 AS
SELECT state_id AS id, ts, dur FROM _idle_freq_materialized WHERE cpu = 0;

CREATE PERFETTO TABLE _stats_cpu1 AS
SELECT state_id AS id, ts, dur FROM _idle_freq_materialized WHERE cpu = 1;

CREATE PERFETTO TABLE _stats_cpu2 AS
SELECT state_id AS id, ts, dur FROM _idle_freq_materialized WHERE cpu = 2;

CREATE PERFETTO TABLE _stats_cpu3 AS
SELECT state_id AS id, ts, dur FROM _idle_freq_materialized WHERE cpu = 3;

CREATE PERFETTO TABLE _stats_cpu4 AS
SELECT state_id AS id, ts, dur FROM _idle_freq_materialized WHERE cpu = 4;

CREATE PERFETTO TABLE _stats_cpu5 AS
SELECT state_id AS id, ts, dur FROM _idle_freq_materialized WHERE cpu = 5;

CREATE PERFETTO TABLE _stats_cpu6 AS
SELECT state_id AS id, ts, dur FROM _idle_freq_materialized WHERE cpu = 6;

CREATE PERFETTO TABLE _stats_cpu7 AS
SELECT state_id AS id, ts, dur FROM _idle_freq_materialized WHERE cpu = 7;

-- Per-interval CPU data, and the last generic table before going to device
-- specific table calcs.
--
-- Because `_stats_cpu0..7` pass `state_id` as their `id`, `_wattson_dsu_frequency`
-- passes `dsu_freq` as its `id`, and `_gapless_suspend_slices` passes
-- `suspended` as its `id`, `interval_intersect` directly outputs those values
-- in `base.id_0..10`. This eliminates 10 out of 11 table joins across 434K
-- intervals.
CREATE PERFETTO TABLE _w_cpu_slices AS
SELECT
  base.ts,
  base.dur,
  cast_int!(l3.l3_hit_rate * base.dur) AS l3_hit_count,
  cast_int!(l3.l3_miss_rate * base.dur) AS l3_miss_count,
  base.id_10 AS suspended,
  base.id_8 AS dsu_freq,
  hash(
    base.id_0,
    base.id_1,
    base.id_2,
    base.id_3,
    base.id_4,
    base.id_5,
    base.id_6,
    base.id_7,
    base.id_8,
    base.id_10
  ) AS config_hash,
  base.id_0 AS state_id_0,
  base.id_1 AS state_id_1,
  base.id_2 AS state_id_2,
  base.id_3 AS state_id_3,
  base.id_4 AS state_id_4,
  base.id_5 AS state_id_5,
  base.id_6 AS state_id_6,
  base.id_7 AS state_id_7
FROM _interval_intersect!(
  (
    _stats_cpu0,
    _stats_cpu1,
    _stats_cpu2,
    _stats_cpu3,
    _stats_cpu4,
    _stats_cpu5,
    _stats_cpu6,
    _stats_cpu7,
    (SELECT dsu_freq AS id, ts, dur FROM _wattson_dsu_frequency),
    _ii_subquery!(_arm_l3_rates),
    (SELECT suspended AS id, ts, dur FROM _gapless_suspend_slices)
  ),
  ()
) AS base
JOIN _arm_l3_rates AS l3
  ON l3._auto_id = base.id_9;

-- Tiny (~400 row) L1/L2-cached lookup table decoding state_id into
-- (freq, idle, curve_value, static) built directly from _adjusted_cpu_freq.
CREATE PERFETTO TABLE _cpu_state_lut AS
WITH
  cpu_freqs AS (
    SELECT DISTINCT cpu, coalesce(freq, 0) AS freq FROM _adjusted_cpu_freq
    UNION
    SELECT 0 AS cpu, 0 AS freq
    UNION
    SELECT 1, 0
    UNION
    SELECT 2, 0
    UNION
    SELECT 3, 0
    UNION
    SELECT 4, 0
    UNION
    SELECT 5, 0
    UNION
    SELECT 6, 0
    UNION
    SELECT 7, 0
  ),
  idles AS (
    SELECT _encode_idle!(-1) AS idle_id, -1 AS idle
    UNION
    SELECT _encode_idle!(0), 0
    UNION
    SELECT _deepest_idle_id!(), idle FROM _deepest_idle
    UNION
    SELECT _offline_idle_id!(), NULL
    UNION
    SELECT _null_idle_id!(), NULL
  )
SELECT
  _pack_cpu_state!(cf.cpu, cf.freq, i.idle_id) AS state_id,
  cf.freq,
  coalesce(i.idle, deepest.idle) AS idle,
  iif(i.idle_id = _offline_idle_id!(), 0, lut.curve_value) AS curve_value,
  iif(cf.cpu IN _device_policies, coalesce(lut.static, 0), 0) AS static
FROM _deepest_idle AS deepest
CROSS JOIN cpu_freqs AS cf
CROSS JOIN idles AS i
LEFT JOIN _dev_cpu_policy_map AS m
  ON m.cpu = cf.cpu
LEFT JOIN _filtered_curves_1d AS lut
  ON lut.policy = m.policy
  AND lut.freq_khz = cf.freq
  AND lut.idle = i.idle;

CREATE PERFETTO INDEX _cpu_state_lut_idx ON _cpu_state_lut(state_id);

-- All UNIQUE configs of independent CPU data.
CREATE PERFETTO TABLE _w_unique_configs AS
SELECT
  reps.config_hash,
  d0.freq AS freq_0,
  d0.idle AS idle_0,
  d1.freq AS freq_1,
  d1.idle AS idle_1,
  d2.freq AS freq_2,
  d2.idle AS idle_2,
  d3.freq AS freq_3,
  d3.idle AS idle_3,
  d4.freq AS freq_4,
  d4.idle AS idle_4,
  d5.freq AS freq_5,
  d5.idle AS idle_5,
  d6.freq AS freq_6,
  d6.idle AS idle_6,
  d7.freq AS freq_7,
  d7.idle AS idle_7,
  d0.curve_value AS cpu0_curve,
  d1.curve_value AS cpu1_curve,
  d2.curve_value AS cpu2_curve,
  d3.curve_value AS cpu3_curve,
  d4.curve_value AS cpu4_curve,
  d5.curve_value AS cpu5_curve,
  d6.curve_value AS cpu6_curve,
  d7.curve_value AS cpu7_curve,
  reps.dsu_freq,
  CAST(_bitmask8!(
    reps.cpus_on_mask & pm.m0,
    reps.cpus_on_mask & pm.m1,
    reps.cpus_on_mask & pm.m2,
    reps.cpus_on_mask & pm.m3,
    reps.cpus_on_mask & pm.m4,
    reps.cpus_on_mask & pm.m5,
    reps.cpus_on_mask & pm.m6,
    reps.cpus_on_mask & pm.m7
  ) AS INTEGER) AS policy_cpus_on_mask,
  iif(reps.cpus_on_mask & pm.m0, d0.static, 0)
  + iif(reps.cpus_on_mask & pm.m1, d1.static, 0)
  + iif(reps.cpus_on_mask & pm.m2, d2.static, 0)
  + iif(reps.cpus_on_mask & pm.m3, d3.static, 0)
  + iif(reps.cpus_on_mask & pm.m4, d4.static, 0)
  + iif(reps.cpus_on_mask & pm.m5, d5.static, 0)
  + iif(reps.cpus_on_mask & pm.m6, d6.static, 0)
  + iif(reps.cpus_on_mask & pm.m7, d7.static, 0) AS static_1d,
  reps.suspended
FROM _policy_masks AS pm
CROSS JOIN (
  SELECT
    config_hash,
    dsu_freq,
    suspended,
    state_id_0,
    state_id_1,
    state_id_2,
    state_id_3,
    state_id_4,
    state_id_5,
    state_id_6,
    state_id_7,
    CAST(_bitmask8!(
      _is_cpu_active!(state_id_0),
      _is_cpu_active!(state_id_1),
      _is_cpu_active!(state_id_2),
      _is_cpu_active!(state_id_3),
      _is_cpu_active!(state_id_4),
      _is_cpu_active!(state_id_5),
      _is_cpu_active!(state_id_6),
      _is_cpu_active!(state_id_7)
    ) AS INTEGER) AS cpus_on_mask
  FROM _w_cpu_slices
  GROUP BY
    config_hash
) AS reps
CROSS JOIN _cpu_state_lut AS d0
  ON d0.state_id = reps.state_id_0
CROSS JOIN _cpu_state_lut AS d1
  ON d1.state_id = reps.state_id_1
CROSS JOIN _cpu_state_lut AS d2
  ON d2.state_id = reps.state_id_2
CROSS JOIN _cpu_state_lut AS d3
  ON d3.state_id = reps.state_id_3
CROSS JOIN _cpu_state_lut AS d4
  ON d4.state_id = reps.state_id_4
CROSS JOIN _cpu_state_lut AS d5
  ON d5.state_id = reps.state_id_5
CROSS JOIN _cpu_state_lut AS d6
  ON d6.state_id = reps.state_id_6
CROSS JOIN _cpu_state_lut AS d7
  ON d7.state_id = reps.state_id_7;

-- Per-interval CPU data with the configuration columns attached. Consumers
-- that only need the per-interval columns should read `_w_cpu_slices`
-- directly, since this view has to join every interval back to its config.
CREATE PERFETTO VIEW _w_independent_cpus_calc AS
SELECT
  slices.ts,
  slices.dur,
  slices.l3_hit_count,
  slices.l3_miss_count,
  slices.config_hash,
  cfg.freq_0,
  cfg.idle_0,
  cfg.freq_1,
  cfg.idle_1,
  cfg.freq_2,
  cfg.idle_2,
  cfg.freq_3,
  cfg.idle_3,
  cfg.freq_4,
  cfg.idle_4,
  cfg.freq_5,
  cfg.idle_5,
  cfg.freq_6,
  cfg.idle_6,
  cfg.freq_7,
  cfg.idle_7,
  cfg.cpu0_curve,
  cfg.cpu1_curve,
  cfg.cpu2_curve,
  cfg.cpu3_curve,
  cfg.cpu4_curve,
  cfg.cpu5_curve,
  cfg.cpu6_curve,
  cfg.cpu7_curve,
  slices.suspended,
  slices.dsu_freq,
  cfg.policy_cpus_on_mask,
  cfg.static_1d
FROM _w_cpu_slices AS slices
JOIN _w_unique_configs AS cfg USING (config_hash);

-- Slices view with all UNIQUE configs of independent and dependent CPU data
CREATE PERFETTO VIEW _w_dependent_cpus_unique AS
WITH
  -- Gets DSU dependent CPU upfront as a single row, which means this can be
  -- efficiently CROSS JOIN-ed later
  dsu_flags AS (
    SELECT
      max(cpu = 0) AS dsu_0,
      max(cpu = 1) AS dsu_1,
      max(cpu = 2) AS dsu_2,
      max(cpu = 3) AS dsu_3,
      max(cpu = 4) AS dsu_4,
      max(cpu = 5) AS dsu_5,
      max(cpu = 6) AS dsu_6,
      max(cpu = 7) AS dsu_7
    FROM _cpu_w_dsu_dependency
  ),
  -- Only unpivot the necessary columns for dependency calculation.
  -- Additionally, only unpivot the necessary rows for dependency calculation
  -- based off of _cpu_lut_dependencies. The superset of the CROSS JOIN will be
  -- CPU (x0, y0), ..., (x0, yN), ..., (xN, yN). The _cpu_lut_dependencies will
  -- eliminate any possible CPU-pairing that are not possible dependencies.
  unpivoted_deps AS (
    SELECT
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
    FROM _w_unique_configs AS i
    CROSS JOIN _cpu_lut_dependencies AS d
    JOIN _dev_vote_by_freq AS v
      ON d.cpu = v.cpu
    JOIN _dev_cpu_policy_map AS p
      ON d.dep_cpu = p.cpu
    WHERE
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
  ),
  max_voters AS (
    SELECT config_hash, cpu, freq, policy, max(vote_score)
    FROM unpivoted_deps
    GROUP BY
      config_hash,
      cpu
  ),
  -- Pivot the results back into new columns.
  pivoted_results AS (
    SELECT
      config_hash,
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
    FROM max_voters
    GROUP BY
      config_hash
  ),
  default_votes AS (
    SELECT
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
    FROM _cpu_w_dependency_default_vote
  )
-- Join the calculated dependencies back to the original data.
SELECT
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
  iif(dsu.dsu_7, 255, coalesce(dep_policy_7, defaults.default_dep_policy_7)) AS dep_policy_7
FROM _w_unique_configs AS base
CROSS JOIN dsu_flags AS dsu
CROSS JOIN default_votes AS defaults
LEFT JOIN pivoted_results AS pivoted USING (config_hash);
