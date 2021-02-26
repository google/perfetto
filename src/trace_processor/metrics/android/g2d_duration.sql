--
-- Copyright 2021 The Android Open Source Project
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

DROP VIEW IF EXISTS raw_g2d_{{g2d_type}}_spans;
CREATE VIEW raw_g2d_{{g2d_type}}_spans AS
SELECT
  ts,
  t.name AS track_name,
  LEAD(ts, 1, 0) OVER (PARTITION BY name ORDER BY ts) - ts AS dur,
  LAG(value, 1, -1) OVER (PARTITION BY name ORDER BY ts) AS prev_g2d_value,
  value AS g2d_value,
  LEAD(value, 1, -1) OVER (PARTITION BY name ORDER BY ts) AS next_g2d_value
FROM counter c JOIN thread_counter_track t ON t.id = c.track_id
WHERE t.name LIKE 'g2d_frame_{{g2d_type}}%';


DROP VIEW IF EXISTS g2d_{{g2d_type}}_spans;
CREATE VIEW g2d_{{g2d_type}}_spans AS
SELECT ts, track_name, dur
FROM raw_g2d_{{g2d_type}}_spans
WHERE g2d_value = 1 AND next_g2d_value = 0;


DROP VIEW IF EXISTS g2d_{{g2d_type}}_errors;
CREATE VIEW g2d_{{g2d_type}}_errors AS
SELECT ts, track_name, g2d_value
FROM raw_g2d_{{g2d_type}}_spans
WHERE (g2d_value = 1 AND next_g2d_value = 1) OR (prev_g2d_value = 0 AND g2d_value = 0);


DROP VIEW IF EXISTS g2d_{{g2d_type}}_instances;
CREATE VIEW g2d_{{g2d_type}}_instances AS
SELECT
  G2dMetrics_G2dInstance(
    'name', g.track_name,
    'max_dur_ns', CAST(MAX(g.dur) AS INT64),
    'min_dur_ns', CAST(MIN(g.dur) AS INT64),
    'avg_dur_ns', CAST(AVG(g.dur) AS INT64),
    'frame_count', COUNT(*),
    'error_count', (SELECT COUNT(*) FROM g2d_{{g2d_type}}_errors e WHERE e.track_name = g.track_name)
  ) AS instance
FROM g2d_{{g2d_type}}_spans g GROUP BY g.track_name;


DROP VIEW IF EXISTS {{output_table}};
CREATE VIEW {{output_table}} AS
SELECT
  G2dMetrics_G2dMetric(
    'instances', (SELECT RepeatedField(instance) FROM g2d_{{g2d_type}}_instances),
    'max_dur_ns', CAST(MAX(dur) AS INT64),
    'min_dur_ns', CAST(MIN(dur) AS INT64),
    'avg_dur_ns', CAST(AVG(dur) AS INT64),
    'frame_count', COUNT(*),
    'error_count', (SELECT COUNT(*) FROM g2d_{{g2d_type}}_errors)
  ) AS metric
FROM g2d_{{g2d_type}}_spans;
