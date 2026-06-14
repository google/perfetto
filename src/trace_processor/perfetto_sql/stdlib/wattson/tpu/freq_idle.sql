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
CREATE PERFETTO PIPELINE _tpu_freq MATERIALIZED AS
SUBPIPELINE nominal_freqs AS (
  FROM slices AS s
  |> JOIN track AS t ON t.id = s.track_id
  |> WHERE t.name = 'DVFS'
  |> SELECT
       s.ts,
       s.dur,
       CAST(s.name AS INTEGER) AS freq,
       CASE
         WHEN s.name GLOB '*Cluster Both*' THEN 2
         WHEN s.name GLOB '*Cluster 0*' THEN 0
         WHEN s.name GLOB '*Cluster 1*' THEN 1
         ELSE 2
       END AS cluster
)
-- Append a zero slice from the last freq event to trace end.
SUBPIPELINE suffix AS (
  FROM nominal_freqs
  |> AGGREGATE
       max(ts) + ARG_MAX(ts, dur) AS ts,
       trace_end() - max(ts) - ARG_MAX(ts, dur) AS dur,
       0 AS freq,
       0 AS cluster
)
-- Prepend a NULL/zero slice up to the first freq event.
FROM nominal_freqs
|> AGGREGATE
     trace_start() AS ts,
     min(ts) - trace_start() AS dur,
     0 AS freq,
     0 AS cluster
|> UNION ALL (
     FROM nominal_freqs
     |> SELECT ts, dur, freq, coalesce(cluster, 2) AS cluster
   )
|> UNION ALL (FROM suffix);

-- Gapless time slices of TPU parallel requests from trace_start() to trace_end()
CREATE PERFETTO PIPELINE _tpu_requests_count MATERIALIZED AS
-- Each 'TPU Requests' slice contributes a +1 at its start and a -1 at its end; a
-- running sum over those deltas is the live concurrency, clamped to [0, 16].
-- Prepend 0 request slices up to first request events
FROM (SELECT trace_start() AS ts, 0 AS delta)
|> UNION ALL (
     -- Request start
     FROM slice AS s
     |> JOIN track AS t ON s.track_id = t.id
     |> WHERE t.name = 'TPU Requests'
     |> SELECT s.ts, 1 AS delta
   )
|> UNION ALL (
     -- Request end (no padding)
     FROM slice AS s
     |> JOIN track AS t ON s.track_id = t.id
     |> WHERE t.name = 'TPU Requests'
     |> SELECT s.ts + s.dur AS ts, -1 AS delta
   )
|> SELECT ts, sum(delta) OVER (ORDER BY ts) AS raw_count
|> AGGREGATE max(raw_count) AS raw_count GROUP BY ts
|> SELECT
     ts,
     lead(ts, 1, trace_end()) OVER (ORDER BY ts) - ts AS dur,
     -- Clamp between 0 and 16 to support higher concurrency tracking
     max(0, min(16, raw_count)) AS requests
|> WHERE dur > 0;
