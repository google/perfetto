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

-- Creates TPU freq counter and cluster based off of slices
-- (gapless from trace_start to trace_end, 0 where no DVFS event defines it)
-- A device without a TPU has no DVFS events, and so no TPU frequency at all.
CREATE PERFETTO TABLE _tpu_freq AS
FROM (
  SELECT
    s.ts,
    iif(s.dur = -1, trace_end() - s.ts, s.dur) AS dur,
    CAST(s.name AS INTEGER) AS freq,
    CASE
      WHEN s.name GLOB '*Cluster Both*' THEN 2
      WHEN s.name GLOB '*Cluster 0*' THEN 0
      WHEN s.name GLOB '*Cluster 1*' THEN 1
      ELSE 2
    END AS cluster
  FROM slice AS s
  JOIN track AS t
    ON t.id = s.track_id
  WHERE
    t.name = 'DVFS'
    AND s.dur != 0
)
|> INTERVAL FILL GAPS WITH (
  SELECT trace_start() AS ts, trace_dur() AS dur, 0 AS freq, 0 AS cluster
  WHERE
    EXISTS (
      SELECT 1
      FROM slice AS s
      JOIN track AS t
        ON t.id = s.track_id
      WHERE
        t.name = 'DVFS'
        AND s.dur != 0
    )
)
|> ORDER BY ts;

-- Number of parallel TPU requests at any point in time. The weight 0
-- background row spanning the whole trace makes INTERVAL FLATTEN also emit
-- the gaps with zero requests.
CREATE PERFETTO TABLE _tpu_requests_flattened AS
FROM (
  SELECT s.ts, s.dur, 1 AS n
  FROM slice AS s
  JOIN track AS t
    ON s.track_id = t.id
  WHERE
    t.name = 'TPU Requests'
    AND s.dur > 0
  UNION ALL
  SELECT trace_start() AS ts, trace_dur() AS dur, 0 AS n
)
|> INTERVAL FLATTEN AGGREGATE SUM(n) AS requests;

-- Gapless time slices of TPU parallel requests from trace_start() to trace_end()
CREATE PERFETTO TABLE _tpu_requests_count AS
-- Clamp to 16 to support higher concurrency tracking
SELECT ts, dur, min(16, requests) AS requests FROM _tpu_requests_flattened;
