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

-- Create all the views used to for LMK related stuff.
CREATE TABLE lmk_events AS
SELECT ref AS upid, MIN(ts) AS ts
FROM instants
WHERE name = 'mem.lmk' AND ref_type = 'upid'
GROUP BY 1;

CREATE VIEW oom_scores AS
SELECT
  ts,
  LEAD(ts, 1, (SELECT end_ts + 1 FROM trace_bounds))
    OVER(PARTITION BY counter_id ORDER BY ts) AS ts_end,
  ref AS upid,
  value AS score
FROM counter_definitions JOIN counter_values USING(counter_id)
WHERE name = 'oom_score_adj' AND ref IS NOT NULL AND ref_type = 'upid';

CREATE VIEW lmk_by_score AS
SELECT lmk_events.upid, CAST(oom_scores.score AS INT) AS score
FROM lmk_events
LEFT JOIN oom_scores
  ON (lmk_events.upid = oom_scores.upid AND
      lmk_events.ts >= oom_scores.ts AND
      lmk_events.ts < oom_scores.ts_end)
ORDER BY lmk_events.upid;

CREATE VIEW lmk_counts AS
SELECT score, COUNT(1) AS count
FROM lmk_by_score
GROUP BY score;

CREATE VIEW android_lmk_output AS
SELECT AndroidLmkMetric(
  'total_count', (SELECT COUNT(1) FROM lmk_events),
  'by_oom_score', (
    SELECT
      RepeatedField(AndroidLmkMetric_ByOomScore(
        'oom_score_adj', score,
        'count', count
      ))
    FROM lmk_counts
  )
);
