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

INCLUDE PERFETTO MODULE common.timestamps;

-- Timestamp of first counter value in a counter.
--
-- @arg counter_track_id  INT Id of a counter track with a counter.
-- @ret LONG                  Timestamp of first counter value. Null if doesn't exist.
CREATE PERFETTO FUNCTION earliest_timestamp_for_counter_track(counter_track_id INT)
RETURNS LONG AS
SELECT MIN(ts) FROM counter WHERE counter.track_id = $counter_track_id;

-- Counter values with details of counter track with calculated duration of each counter value.
-- Duration is calculated as time from counter to the next counter.
--
-- @arg counter_track_id INT   Id of track counter track.
-- @column ts                  Timestamp of the counter value.
-- @column dur                 Duration of the counter value.
-- @column value               Counter value.
-- @column track_id            If of the counter track.
-- @column track_name          Name of the counter track.
-- @column track_arg_set_id    Counter track set id.
-- @column arg_set_id          Counter arg set id.
CREATE PERFETTO FUNCTION counter_with_dur_for_track(counter_track_id INT)
RETURNS TABLE(
    ts LONG,
    dur LONG,
    value DOUBLE,
    track_id INT,
    track_name STRING,
    track_arg_set_id INT,
    arg_set_id INT
) AS
SELECT
  ts,
  LEAD(ts, 1, trace_end()) OVER(ORDER BY ts) - ts AS dur,
  value,
  track.id AS track_id,
  track.name AS track_name,
  track.source_arg_set_id AS track_arg_set_id,
  counter.arg_set_id AS arg_set_id
FROM counter
JOIN counter_track track ON track.id = counter.track_id
WHERE track.id = $counter_track_id;

-- COUNTER_WITH_DUR_FOR_TRACK but in a specified time.
-- Does calculation over the table ends - creates an artificial counter value at
-- the start if needed and chops the duration of the last timestamps in range.
--
-- @arg counter_track_id INT   Id of track counter track.
-- @arg start_ts LONG          Timestamp of the timerange start.
-- Can be earlier than the first counter value.
-- @arg end_ts LONG            Timestamp of the timerange end.
-- @column ts                  Timestamp of the counter value.
-- @column dur                 Duration of the counter value.
-- @column value               Counter value.
-- @column track_id            If of the counter track.
-- @column track_name          Name of the counter track.
-- @column track_arg_set_id    Counter track set id.
-- @column arg_set_id          Counter arg set id.
CREATE PERFETTO FUNCTION COUNTER_FOR_TIME_RANGE(
  counter_track_id INT, start_ts LONG, end_ts LONG)
RETURNS TABLE(
  ts LONG,
  dur LONG,
  value DOUBLE,
  track_id INT,
  track_name STRING,
  track_arg_set_id INT,
  arg_set_id INT
) AS
SELECT
  IIF(ts < $start_ts, $start_ts, ts) AS ts,
  IIF(
    ts < $start_ts,
    dur - ($start_ts - ts),
    IIF(ts + dur > $end_ts, $end_ts - ts, dur)) AS dur,
  value,
  track_id,
  track_name,
  track_arg_set_id,
  arg_set_id
FROM counter_with_dur_for_track($counter_track_id)
WHERE TRUE
  AND ts + dur >= $start_ts
  AND ts < $end_ts
ORDER BY ts ASC;
