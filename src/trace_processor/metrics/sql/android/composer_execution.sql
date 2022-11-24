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


-- The HWC execution time will be calculated based on the runtime of
-- HwcPresentOrValidateDisplay, HwcValidateDisplay, and/or HwcPresentDisplay
-- which are happened in the same onMessageRefresh/composite.
-- There are 3 possible combinations how those functions will be called in
-- a single onMessageRefresh/composite, i.e.:
-- 1. HwcPresentOrValidateDisplay and then HwcPresentDisplay
-- 2. HwcPresentOrValidateDisplay
-- 3. HwcValidateDisplay and then HwcPresentDisplay
DROP VIEW IF EXISTS raw_hwc_function_spans;
CREATE VIEW raw_hwc_function_spans AS
SELECT
  id,
  name,
  ts AS begin_ts,
  ts + dur AS end_ts,
  dur,
  LEAD(name, 1, '') OVER (PARTITION BY track_id ORDER BY ts) AS next_name,
  LEAD(ts, 1, 0) OVER (PARTITION BY track_id ORDER BY ts) AS next_ts,
  LEAD(dur, 1, 0) OVER (PARTITION BY track_id ORDER BY ts) AS next_dur,
  LEAD(name, 2, '') OVER (PARTITION BY track_id ORDER BY ts) AS second_next_name,
  LEAD(ts, 2, 0) OVER (PARTITION BY track_id ORDER BY ts) AS second_next_ts,
  LEAD(dur, 2, 0) OVER (PARTITION BY track_id ORDER BY ts) AS second_next_dur
FROM slice
WHERE name = 'HwcPresentOrValidateDisplay' OR name = 'HwcValidateDisplay'
  OR name = 'HwcPresentDisplay' OR name = 'onMessageRefresh' OR name GLOB 'composite *'
ORDER BY ts;

DROP VIEW IF EXISTS {{output}};
CREATE VIEW {{output}} AS
SELECT
  id,
  CASE
    WHEN begin_ts <= next_ts AND next_ts <= end_ts THEN
      CASE
        WHEN begin_ts <= second_next_ts AND second_next_ts <= end_ts
          THEN next_dur + second_next_dur
        ELSE next_dur
      END
    ELSE 0
  END AS execution_time_ns,
  CASE
    WHEN next_name = 'HwcPresentOrValidateDisplay'
      AND second_next_name = 'HwcPresentDisplay' THEN 'unskipped_validation'
    WHEN next_name = 'HwcPresentOrValidateDisplay'
      AND second_next_name != 'HwcPresentDisplay' THEN 'skipped_validation'
    WHEN next_name = 'HwcValidateDisplay'
      AND second_next_name = 'HwcPresentDisplay' THEN 'separated_validation'
    ELSE 'unknown'
  END AS validation_type
FROM raw_hwc_function_spans
WHERE (name = 'onMessageRefresh' OR name GLOB 'composite *') AND dur > 0;
