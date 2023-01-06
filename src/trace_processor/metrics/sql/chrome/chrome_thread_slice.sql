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

SELECT RUN_METRIC('chrome/chrome_processes.sql');

-- Grab all the thread tracks which are found in chrome threads.
DROP VIEW IF EXISTS chrome_track;
CREATE VIEW chrome_track AS
SELECT
  *
FROM thread_track
WHERE utid IN (SELECT utid FROM chrome_thread);

-- From all the chrome thread tracks select all the slice details for thread
-- slices.
DROP VIEW IF EXISTS chrome_thread_slice;
CREATE VIEW chrome_thread_slice AS
SELECT
  slice.*
FROM
  slice JOIN
  chrome_track ON
    chrome_track.id = slice.track_id
WHERE
  track_id IN (SELECT id FROM chrome_track);
