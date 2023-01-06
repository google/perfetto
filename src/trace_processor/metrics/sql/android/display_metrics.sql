--
-- Copyright 2020 The Android Open Source Project
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
--
DROP VIEW IF EXISTS same_frame;
CREATE VIEW same_frame AS
SELECT COUNT(name) AS total_duplicate_frames
FROM counters
WHERE name = 'SAME_FRAME'
  AND value = 1;

DROP VIEW IF EXISTS duplicate_frames_logged;
CREATE VIEW duplicate_frames_logged AS
SELECT CASE WHEN COUNT(name) > 0 THEN 1 ELSE 0 END AS logs_found
FROM counters
WHERE name = 'SAME_FRAME' AND value = 0;

DROP VIEW IF EXISTS dpu_underrun;
CREATE VIEW dpu_underrun AS
SELECT COUNT(name) AS total_dpu_underrun_count
FROM counters
WHERE name = 'DPU_UNDERRUN'
  AND value = 1;

DROP VIEW IF EXISTS non_repeated_panel_fps;
CREATE VIEW non_repeated_panel_fps AS
SELECT *
FROM (
  SELECT
    ts,
    value,
    track_id,
    LAG(value, 1, 0) OVER (PARTITION BY track_id ORDER BY ts) AS prev_value
  FROM counter c JOIN track t ON c.track_id = t.id
  WHERE t.name = 'panel_fps'
  ORDER BY ts
)
WHERE prev_value != value;

DROP VIEW IF EXISTS panel_fps_spans;
CREATE VIEW panel_fps_spans AS
SELECT *
FROM (
  SELECT
    ts,
    value,
    LEAD(ts) OVER (PARTITION BY track_id ORDER BY ts) - ts AS dur
  FROM non_repeated_panel_fps
  ORDER BY ts
)
WHERE dur > 0;

DROP VIEW IF EXISTS update_power_state_stats;
CREATE VIEW update_power_state_stats AS
SELECT
  CAST(AVG(dur) / 1e3 AS INT64) AS avg_runtime_micro_secs
FROM slice
WHERE slice.name = 'DisplayPowerController#updatePowerState' AND slice.dur >= 0;

DROP VIEW IF EXISTS display_metrics_output;
CREATE VIEW display_metrics_output AS
SELECT AndroidDisplayMetrics(
  'total_duplicate_frames', (SELECT total_duplicate_frames
    FROM same_frame),
  'duplicate_frames_logged', (SELECT logs_found
    FROM duplicate_frames_logged),
  'total_dpu_underrun_count', (SELECT total_dpu_underrun_count
    FROM dpu_underrun),
  'refresh_rate_switches', (SELECT COUNT(*) FROM panel_fps_spans),
  'refresh_rate_stats', (
    SELECT RepeatedField(metric)
    FROM (
      SELECT AndroidDisplayMetrics_RefreshRateStat(
        'refresh_rate_fps', CAST(value AS UINT32),
        'count', COUNT(*),
        'total_dur_ms', SUM(dur) / 1e6,
        'avg_dur_ms', AVG(dur) / 1e6
      ) AS metric
      FROM panel_fps_spans
      GROUP BY value
      ORDER BY value
    )
  ),
  'update_power_state', (
    SELECT AndroidDisplayMetrics_UpdatePowerState(
      'avg_runtime_micro_secs', avg_runtime_micro_secs
    )
    FROM update_power_state_stats
  )
);
