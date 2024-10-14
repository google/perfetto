--
-- Copyright 2019 The Android Open Source Project
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

SELECT RUN_METRIC('android/process_oom_score.sql');

-- All LMK events ordered by timestamp
DROP TABLE IF EXISTS lmk_events;
CREATE PERFETTO TABLE lmk_events AS
WITH raw_events AS (
  SELECT upid, MAX(ts) AS ts
  FROM instant
  JOIN process_track ON instant.track_id = process_track.id
  WHERE instant.name = 'mem.lmk'
  GROUP BY 1
)
SELECT
  raw_events.ts,
  raw_events.upid,
  oom_scores.oom_score_val AS score
FROM raw_events
LEFT JOIN oom_score_span oom_scores
  ON (raw_events.upid = oom_scores.upid
      AND raw_events.ts >= oom_scores.ts
      AND raw_events.ts < oom_scores.ts + oom_scores.dur)
ORDER BY 1;

DROP VIEW IF EXISTS android_lmk_output;
CREATE PERFETTO VIEW android_lmk_output AS
WITH lmk_counts AS (
  SELECT score, COUNT(1) AS count
  FROM lmk_events
  GROUP BY score
)
SELECT AndroidLmkMetric(
  'total_count', (SELECT COUNT(1) FROM lmk_events),
  'by_oom_score', (
    SELECT
      RepeatedField(AndroidLmkMetric_ByOomScore(
        'oom_score_adj', score,
        'count', count
      ))
    FROM lmk_counts
    WHERE score IS NOT NULL
  ),
  'oom_victim_count', (
    SELECT COUNT(1)
    FROM instant
    WHERE name = 'mem.oom_kill'
  )
);
