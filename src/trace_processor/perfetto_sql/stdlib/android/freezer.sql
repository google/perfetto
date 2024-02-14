--
-- Copyright 2024 The Android Open Source Project
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

CREATE PERFETTO FUNCTION _extract_freezer_pid(name STRING)
RETURNS INT
AS
SELECT CAST(reverse(str_split(reverse(str_split($name, ' ', 1)), ':', 0)) AS INT);

-- Converts a pid to a upid using the timestamp of occurence of an event from
-- |pid| to disambiguate duplicate pids.
--
-- This is still best effort because it relies on having information about
-- process start and end in the trace. In the edge case that we are missing this,
-- it best effort returns the last upid.
CREATE PERFETTO FUNCTION _pid_to_upid(
  -- Pid to convert from.
  pid INT,
  -- Timestamp of an event from the |pid|.
  event_ts INT)
-- Returns the converted upid.
RETURNS INT
AS
WITH
  process_lifetime AS (
    SELECT
      pid,
      upid,
      COALESCE(start_ts, trace_start()) AS start_ts,
      COALESCE(end_ts, trace_end()) AS end_ts
    FROM process
  )
SELECT upid
FROM process_lifetime
WHERE pid = $pid AND $event_ts BETWEEN start_ts AND end_ts
ORDER BY upid DESC
LIMIT 1;

-- All frozen processes and their frozen duration.
CREATE PERFETTO TABLE android_freezer_events (
  -- Upid of frozen process
  upid INT,
  -- Pid of frozen process
  pid INT,
  -- Timestamp process was frozen.
  ts INT,
  -- Duration process was frozen for.
  dur INT
  )
AS
WITH
  freeze AS (
    SELECT ts, _extract_freezer_pid(name) AS pid,
    _pid_to_upid(_extract_freezer_pid(name), ts) AS upid, 'freeze' AS type
    FROM slice
    WHERE name GLOB 'Freeze *:*'
  ),
  unfreeze AS (
    SELECT ts, _extract_freezer_pid(name) AS pid,
    _pid_to_upid(_extract_freezer_pid(name), ts) AS upid, 'unfreeze' AS type
    FROM slice
    WHERE name GLOB 'Unfreeze *:*'
  ),
  merged AS (
    SELECT * FROM freeze
    UNION ALL
    SELECT * FROM unfreeze
  ),
  starts AS (
    SELECT
      type,
      upid,
      pid,
      ts,
      ifnull(lead(ts) OVER (PARTITION BY upid ORDER BY ts), trace_end()) - ts AS dur
    FROM merged
  )
SELECT upid, pid, ts, dur
FROM starts
WHERE starts.type = 'freeze' AND upid IS NOT NULL;
