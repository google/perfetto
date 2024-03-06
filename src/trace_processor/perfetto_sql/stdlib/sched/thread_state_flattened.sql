--
-- Copyright 2023 The Android Open Source Project
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

INCLUDE PERFETTO MODULE slices.flat_slices;

-- Create a table which joins the thread state across the flattened slices.
CREATE VIRTUAL TABLE __span_joined_thread USING
  SPAN_JOIN(_slice_flattened PARTITIONED utid, thread_state PARTITIONED utid);

-- Get the thread state breakdown of a flattened slice from its slice id.
-- This table pivoted and summed for better visualization and aggregation.
-- The concept of a "flat slice" is to take the data in the slice table and
-- remove all notion of nesting. For more information, read the description
-- of _slice_flattened.
CREATE PERFETTO FUNCTION _get_flattened_thread_state(
  -- Id of the slice of interest.
  slice_id LONG,
  -- Utid.
  utid LONG)
RETURNS
  TABLE(
    -- Timestamp.
    ts LONG,
    -- Duration.
    dur LONG,
    -- Utid.
    utid LONG,
    -- Depth.
    depth LONG,
    -- Name.
    name STRING,
    -- Slice id.
    slice_id LONG,
    -- Track id.
    track_id LONG,
    -- CPU.
    cpu INT,
    -- State.
    state STRING,
    -- IO wait.
    io_wait INT,
    -- Thread state's blocked_function.
    blocked_function STRING,
    -- Thread state's waker utid.
    waker_utid LONG,
    -- Thread state's IRQ context.
    irq_context LONG
) AS
WITH
interesting_slice AS (
  SELECT ts, dur, slice.track_id AS track_id
  FROM slice
  JOIN thread_track
    ON slice.track_id = thread_track.id
  JOIN thread
    USING (utid)
  WHERE
    (($slice_id IS NOT NULL AND slice.id = $slice_id) OR ($slice_id IS NULL))
    AND (($utid IS NOT NULL AND utid = $utid) OR ($utid IS NULL))
)
SELECT
  ts,
  dur,
  utid,
  depth,
  name,
  slice_id,
  track_id,
  cpu,
  state,
  io_wait,
  blocked_function,
  waker_utid,
  irq_context
FROM __span_joined_thread
WHERE
  track_id = (SELECT track_id FROM interesting_slice)
  AND ts >= (SELECT ts FROM interesting_slice)
  AND ts < (SELECT ts + dur FROM interesting_slice);

-- Get the thread state breakdown of a flattened slice from slice id.
-- This table pivoted and summed for better visualization and aggragation.
-- The concept of a "flat slice" is to take the data in the slice table and
-- remove all notion of nesting. For more information, read the description
-- of _slice_flattened.
CREATE PERFETTO FUNCTION _get_flattened_thread_state_aggregated(
  -- Slice id.
  slice_id LONG,
  -- Utid.
  utid LONG)
RETURNS TABLE(
  -- Id of a slice.
  slice_id LONG,
  -- Name of the slice.
  slice_name STRING,
  -- Time (ns) spent in Uninterruptible Sleep (non-IO)
  Uninterruptible_Sleep_nonIO LONG,
  -- Time (ns) spent in Uninterruptible Sleep (IO)
  Uninterruptible_Sleep_IO LONG,
  -- Time (ns) spent in Runnable
  Runnable LONG,
  -- Time (ns) spent in Sleeping
  Sleeping LONG,
  -- Time (ns) spent in Stopped
  Stopped LONG,
  -- Time (ns) spent in Traced
  Traced LONG,
  -- Time (ns) spent in Exit (Dead)
  Exit_Dead LONG,
  -- Time (ns) spent in Exit (Zombie)
  Exit_Zombie LONG,
  -- Time (ns) spent in Task Dead
  Task_Dead LONG,
  -- Time (ns) spent in Wake Kill
  Wake_Kill LONG,
  -- Time (ns) spent in Waking
  Waking LONG,
  -- Time (ns) spent in Parked
  Parked LONG,
  -- Time (ns) spent in No Load
  No_Load LONG,
  -- Time (ns) spent in Runnable (Preempted)
  Runnable_Preempted LONG,
  -- Time (ns) spent in Running
  Running LONG,
  -- Time (ns) spent in Idle
  Idle LONG,
  -- Total duration of the slice
  dur LONG,
  -- Depth of the slice in Perfetto
  depth LONG)
AS
WITH
final_table AS (
  SELECT *
  FROM _get_flattened_thread_state($slice_id, $utid)
)
SELECT
fs.slice_id,
fs.name AS slice_name,
SUM(CASE WHEN fs.state = 'D' AND io_wait = 0 THEN fs.dur END)
  Uninterruptible_Sleep_nonIO,
SUM(CASE WHEN fs.state = 'D' AND io_wait = 1 THEN fs.dur END)
  Uninterruptible_Sleep_IO,
SUM(CASE WHEN fs.state = 'R' THEN fs.dur END) Runnable,
SUM(CASE WHEN fs.state = 'S' THEN fs.dur END) Sleeping,
SUM(CASE WHEN fs.state = 'T' THEN fs.dur END) Stopped,
SUM(CASE WHEN fs.state = 't' THEN fs.dur END) Traced,
SUM(CASE WHEN fs.state = 'X' THEN fs.dur END) Exit_Dead,
SUM(CASE WHEN fs.state = 'Z' THEN fs.dur END) Exit_Zombie,
SUM(CASE WHEN fs.state = 'x' THEN fs.dur END) Task_Dead,
SUM(CASE WHEN fs.state = 'K' THEN fs.dur END) Wake_Kill,
SUM(CASE WHEN fs.state = 'W' THEN fs.dur END) Waking,
SUM(CASE WHEN fs.state = 'P' THEN fs.dur END) Parked,
SUM(CASE WHEN fs.state = 'N' THEN fs.dur END) No_Load,
SUM(CASE WHEN fs.state = 'R+' THEN fs.dur END) Runnable_Preempted,
SUM(CASE WHEN fs.state = 'Running' THEN fs.dur END) Running,
SUM(CASE WHEN fs.state = 'I' THEN fs.dur END) Idle,
SUM(fs.dur) dur,
fs.depth
FROM final_table fs
GROUP BY fs.slice_id;