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
--

-- All activity launches in the trace, keyed by ID.
DROP TABLE IF EXISTS launches;
CREATE TABLE launches(
  id INTEGER PRIMARY KEY,
  ts BIG INT,
  ts_end BIG INT,
  dur BIG INT,
  package STRING
);

-- Cold/warm starts emitted launching slices on API level 28-.
INSERT INTO launches(ts, ts_end, dur, package)
SELECT
  launching_events.ts AS ts,
  launching_events.ts_end AS ts_end,
  launching_events.ts_end - launching_events.ts AS dur,
  package_name AS package
FROM launching_events;

-- TODO(lalitm): add handling of hot starts using frame timings.
