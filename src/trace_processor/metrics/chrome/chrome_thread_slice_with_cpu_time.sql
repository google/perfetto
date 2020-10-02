--
-- Copyright 2020 The Android Open Source Project
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

-- From all the chrome thread tracks select all the slice details as well as
-- the utid of the track so we can join with counter table later.
DROP VIEW IF EXISTS chrome_slice;

CREATE VIEW chrome_slice AS
  SELECT
    slice.*,
    chrome_track.utid
  FROM
    slice JOIN
    chrome_track ON
        chrome_track.id = slice.track_id
  WHERE
    track_id in (SELECT id FROM chrome_track);

-- Using utid join the thread_counter_track to chrome thread slices. This allows
-- the filtering of the counter table to only counters associated to these
-- threads.
DROP VIEW IF EXISTS chrome_slice_and_counter_track;

CREATE VIEW chrome_slice_and_counter_track AS
  SELECT
    s.*,
    thread_counter_track.id as counter_track_id,
    thread_counter_track.name as counter_name
  FROM
    chrome_slice s JOIN
    thread_counter_track ON
        thread_counter_track.utid = s.utid AND
        thread_counter_track.name = "thread_time";

-- Join each slice with the recorded value at the beginning and the end, as
-- well as computing the total CPU time each slice took.
--
-- We use MIN and MAX inside because sometimes nested slices will have the exact
-- same timestamp and we need to select one, there is nothing tying a particular
-- counter value to which slice generated it so we always choose the minimum for
-- the start on ties and the maximum for ties on the end of the slice. This
-- means this is always an overestimate, but events being emitted at exactly the
-- same timestamp is relatively rare so shouldn't cause to much inflation.
DROP VIEW IF EXISTS chrome_thread_slice_with_cpu_time;

CREATE VIEW chrome_thread_slice_with_cpu_time AS
  SELECT
    end_cpu_time - start_cpu_time AS slice_cpu_time,
    *
  FROM (
    SELECT
      s.*,
      min_counter.start_cpu_time
    FROM
      chrome_slice_and_counter_track s LEFT JOIN (
        SELECT
          ts,
          track_id,
          MIN(value) AS start_cpu_time
        FROM counter
        GROUP BY 1, 2
      ) min_counter ON
          min_counter.ts = s.ts AND min_counter.track_id = s.counter_track_id
  ) min_and_slice LEFT JOIN (
    SELECT
      ts,
      track_id,
      MAX(value) AS end_cpu_time
    FROM counter
    GROUP BY 1, 2
  ) max_counter ON
      max_counter.ts =
          CASE WHEN min_and_slice.dur >= 0 THEN
            min_and_slice.ts + min_and_slice.dur
          ELSE
            min_and_slice.ts
          END AND
      max_counter.track_id = min_and_slice.counter_track_id;
