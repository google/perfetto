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

INCLUDE PERFETTO MODULE experimental.flat_slices;

-- Create a table which joins the thread state across the flattened slices.
CREATE VIRTUAL TABLE internal_experimental_span_joined_thread USING
  SPAN_JOIN(experimental_slice_flattened PARTITIONED utid, thread_state PARTITIONED utid);

-- Get the thread state breakdown of a flattened slice from it's slice id.
-- This table pivoted and summed for better visualization and aggragation.
-- The concept of a "flat slice" is to take the data in the slice table and
-- remove all notion of nesting. For more information, read the description
-- of experimental_slice_flattened.
--
-- @arg slice_id LONG         Id of the slice of interest.
--
-- @column ts                 Id of a slice.
-- @column dur                Name of the slice
-- @column utid               Time (ns) spent in Uninterruptible Sleep (non-IO)
-- @column depth              Time (ns) spent in Uninterruptible Sleep (IO)
-- @column name               Time (ns) spent in Runnable
-- @column slice_id           Time (ns) spent in Sleeping
-- @column track_id           Time (ns) spent in Stopped
-- @column cpu                Time (ns) spent in Exit (Zombie)
-- @column state              Time (ns) spent in Task Dead
-- @column io_wait            Time (ns) spent in Wake Kill
-- @column blocked_function   Time (ns) spent in Waking
-- @column waker_utid         Time (ns) spent in Parked
-- @column irq_context        Time (ns) spent in No Load
CREATE PERFETTO FUNCTION experimental_get_flattened_thread_state(
  slice_id LONG, utid LONG)
RETURNS
  TABLE(
    ts LONG,
    dur LONG,
    utid LONG,
    depth LONG,
    name STRING,
    slice_id LONG,
    track_id LONG,
    cpu INT,
    state STRING,
    io_wait INT,
    blocked_function STRING,
    waker_utid LONG,
    irq_context LONG)
AS
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
FROM internal_experimental_span_joined_thread
WHERE
  track_id = (SELECT track_id FROM interesting_slice)
  AND ts >= (SELECT ts FROM interesting_slice)
  AND ts < (SELECT ts + dur FROM interesting_slice);

-- Get the thread state breakdown of a flattened slice from slice id.
-- This table pivoted and summed for better visualization and aggragation.
-- The concept of a "flat slice" is to take the data in the slice table and
-- remove all notion of nesting. For more information, read the description
-- of experimental_slice_flattened.
--
-- @arg slice_id LONG                  Id of the slice of interest.
--
-- @column slice_id                    Id of a slice.
-- @column slice_name                  Name of the slice
-- @column Uninterruptible_Sleep_nonIO Time (ns) spent in Uninterruptible Sleep (non-IO)
-- @column Uninterruptible_Sleep_IO    Time (ns) spent in Uninterruptible Sleep (IO)
-- @column Runnable                    Time (ns) spent in Runnable
-- @column Sleeping                    Time (ns) spent in Sleeping
-- @column Stopped                     Time (ns) spent in Stopped
-- @column Traced                      Time (ns) spent in Traced
-- @column Exit_Dead                   Time (ns) spent in Exit (Dead)
-- @column Exit_Zombie                 Time (ns) spent in Exit (Zombie)
-- @column Task_Dead                   Time (ns) spent in Task Dead
-- @column Wake_Kill                   Time (ns) spent in Wake Kill
-- @column Waking                      Time (ns) spent in Waking
-- @column Parked                      Time (ns) spent in Parked
-- @column No_Load                     Time (ns) spent in No Load
-- @column Runnable_Preempted          Time (ns) spent in Runnable (Preempted)
-- @column Running                     Time (ns) spent in Running
-- @column Idle                        Time (ns) spent in Idle
-- @column dur                         Total duration of the slice
-- @column depth                       Depth of the slice in Perfetto
CREATE PERFETTO FUNCTION experimental_get_flattened_thread_state_aggregated(
  slice_id LONG, utid LONG)
RETURNS
  TABLE(
    slice_id LONG,
    slice_name STRING,
    Uninterruptible_Sleep_nonIO LONG,
    Uninterruptible_Sleep_IO LONG,
    Runnable LONG,
    Sleeping LONG,
    Stopped LONG,
    Traced LONG,
    Exit_Dead LONG,
    Exit_Zombie LONG,
    Task_Dead LONG,
    Wake_Kill LONG,
    Waking LONG,
    Parked LONG,
    No_Load LONG,
    Runnable_Preempted LONG,
    Running LONG,
    Idle LONG,
    dur LONG,
    depth LONG)
AS
WITH
  final_table AS (
    SELECT *
    FROM experimental_get_flattened_thread_state($slice_id, $utid)
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