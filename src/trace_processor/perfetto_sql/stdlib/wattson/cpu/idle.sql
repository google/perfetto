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

INCLUDE PERFETTO MODULE counters.intervals;

INCLUDE PERFETTO MODULE wattson.device_infos;

-- Pre-map cpuidle counter track IDs to CPU and deep-idle offset so that
-- interval generation and offset lookup require a single 8-row index join.
CREATE PERFETTO TABLE _track_idle_offsets AS
SELECT cct.id AS track_id, cct.cpu, offsets.offset_ns
FROM cpu_counter_track AS cct
JOIN _device_cpu_deep_idle_offsets AS offsets
  ON offsets.cpu = cct.cpu
JOIN _wattson_device AS device
  ON offsets.device = device.name
WHERE
  cct.name = 'cpuidle';

CREATE PERFETTO INDEX track_idle_idx ON _track_idle_offsets(track_id);

-- Deep idle slices immediately followed by an idle exit, keyed by exit ts.
CREATE PERFETTO TABLE _deep_idle_before_exit AS
SELECT i.track_id, i.ts + i.dur AS exit_ts, i.ts AS prev_ts
FROM counter_leading_intervals!((
    SELECT c.id, c.ts, c.track_id, c.value
    FROM _track_idle_offsets AS t
    JOIN counter AS c ON c.track_id = t.track_id
    WHERE t.offset_ns > 0
  )) AS i
CROSS JOIN _deepest_idle AS deepest
WHERE
  i.value = deepest.idle
  AND i.next_value = 4294967295;

CREATE PERFETTO INDEX deep_idle_before_exit_idx ON _deep_idle_before_exit(
  track_id,
  exit_ts
);

-- Adjust duration of active portion to be slightly longer to account for
-- overhead cost of transitioning out of deep idle. This is done because the
-- device is active and consumes power for longer than the logs actually report.
CREATE PERFETTO TABLE _adjusted_deep_idle AS
WITH
  first_cpu_ts AS MATERIALIZED (
    SELECT
      m.cpu,
      coalesce(
        (
          SELECT c.ts
          FROM _track_idle_offsets AS t
          JOIN counter AS c
            ON c.track_id = t.track_id
          WHERE
            t.cpu = m.cpu
          ORDER BY
            c.ts
          LIMIT 1
        ),
        trace_end()
      ) AS first_ts
    FROM _dev_cpu_policy_map AS m
  ),
  idle_mod AS MATERIALIZED (
    SELECT
      iif(
        t.offset_ns > 0
        AND i.value - i.delta_value = deepest.idle
        AND i.value = 4294967295,
        -- extend ts backwards by offset_ns at most up to prev_ts
        max(
          i.ts - t.offset_ns,
          (
            SELECT p.prev_ts
            FROM _deep_idle_before_exit AS p
            WHERE
              p.track_id = i.track_id
              AND p.exit_ts = i.ts
          )
        ),
        i.ts
      ) AS ts,
      -- The same adjustment applied to the following transition, which is where
      -- this slice ends.
      iif(
        i.value = deepest.idle
        AND i.next_value = 4294967295,
        max(i.ts + i.dur - t.offset_ns, i.ts),
        i.ts + i.dur
      ) AS end_ts,
      t.cpu,
      cast_int!(i.value) AS idle
    FROM counter_leading_intervals!((
        SELECT c.id, c.ts, c.track_id, c.value
        FROM _track_idle_offsets AS t
        JOIN counter AS c ON c.track_id = t.track_id
      )) AS i
    JOIN _track_idle_offsets AS t
      ON t.track_id = i.track_id
    CROSS JOIN _deepest_idle AS deepest
  )
-- Prepend NULL slices up to first idle events on a per CPU basis
SELECT trace_start() AS ts, first_ts - trace_start() AS dur, cpu, NULL AS idle
FROM first_cpu_ts
WHERE
  first_ts > trace_start()
UNION ALL
SELECT
  m.ts,
  m.end_ts - m.ts AS dur,
  m.cpu,
  coalesce(idle_map.override_idle, iif(m.idle = 4294967295, -1, m.idle)) AS idle
FROM idle_mod AS m
LEFT JOIN _idle_state_map_override AS idle_map
  ON m.idle = idle_map.nominal_idle
WHERE
  m.end_ts > m.ts;
