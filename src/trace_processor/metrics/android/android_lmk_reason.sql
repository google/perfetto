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

SELECT RUN_METRIC('android/android_ion.sql');
SELECT RUN_METRIC('android/android_lmk.sql');

CREATE VIEW IF NOT EXISTS android_lmk_reason_output AS
WITH
lmk_ooms AS (
  SELECT
  lmk_events.ts,
  oom_score_val
  FROM lmk_events
  JOIN oom_score_span USING (upid)
  WHERE lmk_events.ts
    BETWEEN oom_score_span.ts
    AND oom_score_span.ts + MAX(oom_score_span.dur - 1, 0)
),
lmk_ion_sizes AS (
  SELECT
  lmk_events.ts,
  CAST(ion_timeline.value as int) system_ion_heap_size
  FROM lmk_events
  JOIN ion_timeline
  WHERE ion_timeline.heap_name = 'system'
  AND lmk_events.ts
    BETWEEN ion_timeline.ts
    AND ion_timeline.ts + MAX(ion_timeline.dur - 1, 0)
)
SELECT AndroidLmkReasonMetric(
  'lmks', (
    SELECT RepeatedField(AndroidLmkReasonMetric_Lmk(
      'oom_score_adj', oom_score_val,
      'system_ion_heap_size', system_ion_heap_size
    ))
    FROM lmk_events
    LEFT JOIN lmk_ooms USING (ts)
    LEFT JOIN lmk_ion_sizes USING (ts)
    ORDER BY ts
  )
);
