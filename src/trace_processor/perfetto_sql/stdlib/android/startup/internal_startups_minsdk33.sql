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


CREATE VIEW internal_startup_async_events AS
SELECT
  ts,
  dur,
  SUBSTR(name, 19) AS startup_id
FROM slice
WHERE
  name GLOB 'launchingActivity#*'
  AND dur != 0
  AND INSTR(name, ':') = 0;

CREATE VIEW internal_startup_complete_events AS
SELECT
  STR_SPLIT(completed, ':', 0) AS startup_id,
  STR_SPLIT(completed, ':', 2) AS package_name,
  CASE
    WHEN STR_SPLIT(completed, ':', 1) = 'completed-hot' THEN 'hot'
    WHEN STR_SPLIT(completed, ':', 1) = 'completed-warm' THEN 'warm'
    WHEN STR_SPLIT(completed, ':', 1) = 'completed-cold' THEN 'cold'
    ELSE NULL
  END AS startup_type,
  MIN(ts)
FROM (
  SELECT ts, SUBSTR(name, 19) AS completed
  FROM slice
  WHERE
    dur = 0
    -- Originally completed was unqualified, but at some point we introduced
    -- the startup type as well
    AND name GLOB 'launchingActivity#*:completed*:*'
)
GROUP BY 1, 2, 3;

INSERT INTO internal_all_startups
SELECT
  "minsdk33",
  startup_id,
  ts,
  ts + dur AS ts_end,
  dur,
  package_name,
  startup_type
FROM internal_startup_async_events
JOIN internal_startup_complete_events USING (startup_id);



