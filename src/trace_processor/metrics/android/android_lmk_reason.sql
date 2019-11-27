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

SELECT RUN_METRIC('android/android_lmk.sql');

CREATE VIEW IF NOT EXISTS android_lmk_reason_output AS
WITH
lmk_ooms AS (
  SELECT
  lmk_events.ts,
  oom_score_val
  FROM lmk_events
  LEFT JOIN oom_score_span USING (upid)
  WHERE oom_score_span.ts <= lmk_events.ts
  AND (oom_score_span.ts + oom_score_span.dur) > lmk_events.ts
)
SELECT AndroidLmkReasonMetric(
  'lmks', (
    SELECT RepeatedField(AndroidLmkReasonMetric_Lmk(
      'oom_score_adj', oom_score_val
    ))
    FROM lmk_events
    JOIN lmk_ooms USING (ts)
    ORDER BY ts
  )
);
