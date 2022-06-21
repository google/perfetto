--
-- Copyright 2022 The Android Open Source Project
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

DROP VIEW IF EXISTS launch_async_events;
CREATE VIEW launch_async_events AS
SELECT
  ts,
  dur,
  SUBSTR(name, 19) id
FROM slice
WHERE
  name GLOB 'launchingActivity#*' AND
  dur != 0 AND
  INSTR(name, ':') = 0;

DROP VIEW IF EXISTS launch_complete_events;
CREATE VIEW launch_complete_events AS
SELECT
  STR_SPLIT(completed, ':completed:', 0) id,
  STR_SPLIT(completed, ':completed:', 1) package_name,
  MIN(ts)
FROM (
  SELECT ts, SUBSTR(name, 19) completed
  FROM slice
  WHERE
    dur = 0 AND
    name GLOB 'launchingActivity#*:completed:*'
)
GROUP BY 1, 2;

INSERT INTO launches(id, ts, ts_end, dur, package)
SELECT
  id,
  ts,
  ts + dur ts_end,
  dur,
  package_name
FROM launch_async_events
JOIN launch_complete_events USING (id);
