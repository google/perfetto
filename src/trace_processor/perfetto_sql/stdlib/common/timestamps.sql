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
-- Trace bounds
--

-- Fetch start of the trace.
-- @ret LONG  Start of the trace in nanoseconds.
CREATE PERFETTO FUNCTION trace_start()
RETURNS LONG AS
SELECT start_ts FROM trace_bounds;

-- Fetch end of the trace.
-- @ret LONG  End of the trace in nanoseconds.
CREATE PERFETTO FUNCTION trace_end()
RETURNS LONG AS
SELECT end_ts FROM trace_bounds;

-- Fetch duration of the trace.
-- @ret LONG  Duration of the trace in nanoseconds.
CREATE PERFETTO FUNCTION trace_dur()
RETURNS LONG AS
SELECT trace_end() - trace_start();

-- Checks whether two spans are overlapping.
--
-- @arg ts1 LONG      Start of first span.
-- @arg ts_end1 LONG  End of first span.
-- @arg ts2 LONG      Start of second span.
-- @arg ts_end2 LONG  End of second span.
-- @ret BOOL          Whether two spans are overlapping.
CREATE PERFETTO FUNCTION is_spans_overlapping(ts1 LONG, ts_end1 LONG, ts2 LONG, ts_end2 LONG)
RETURNS BOOL AS
SELECT (IIF($ts1 < $ts2, $ts2, $ts1)
      < IIF($ts_end1 < $ts_end2, $ts_end1, $ts_end2));

--Return the overlapping duration between two spans.
--If either duration is less than 0 or there's no intersection, 0 is returned
--
-- @arg ts1 LONG Timestamp of first slice start.
-- @arg dur1 LONG Duration of first slice.
-- @arg ts2 LONG Timestamp of second slice start.
-- @arg dur2 LONG Duration of second slice.
-- @ret INT               Overlapping duration
CREATE PERFETTO FUNCTION spans_overlapping_dur(ts1 LONG, dur1 LONG, ts2 LONG, dur2 LONG)
RETURNS INT AS
SELECT
  CASE
    WHEN $dur1 = -1 OR $dur2 = -1 THEN 0
    WHEN $ts1 + $dur1 < $ts2 OR $ts2 + $dur2 < $ts1 THEN 0
    WHEN ($ts1 >= $ts2) AND ($ts1 + $dur1 <= $ts2 + $dur2) THEN $dur1
    WHEN ($ts1 < $ts2) AND ($ts1 + $dur1 < $ts2 + $dur2) THEN $ts1 + $dur1 - $ts2
    WHEN ($ts1 > $ts2) AND ($ts1 + $dur1 > $ts2 + $dur2) THEN $ts2 + $dur2 - $ts1
    ELSE $dur2
  END;

--
-- Helpers for defining time durations.
--

-- Converts a duration in seconds to nanoseconds, which is the default representation
-- of time durations in trace processor. Provided for consisensy with other functions.
-- @arg nanos INT  Time duration in seconds.
-- @ret INT        Time duration in nanoseconds.
CREATE PERFETTO FUNCTION ns(nanos INT) RETURNS INT AS
SELECT $nanos;

-- Converts a duration in microseconds to nanoseconds, which is the default
-- representation of time durations in trace processor.
-- @arg micros INT  Time duration in microseconds.
-- @ret INT         Time duration in nanoseconds.
CREATE PERFETTO FUNCTION us(micros INT) RETURNS INT AS
SELECT $micros * 1000;

-- Converts a duration in millseconds to nanoseconds, which is the default
-- representation of time durations in trace processor.
-- @arg millis INT  Time duration in milliseconds.
-- @ret INT         Time duration in nanoseconds.
CREATE PERFETTO FUNCTION ms(millis INT) RETURNS INT AS
SELECT $millis * 1000 * 1000;

-- Converts a duration in seconds to nanoseconds, which is the default
-- representation of time durations in trace processor.
-- @arg seconds INT  Time duration in seconds.
-- @ret INT          Time duration in nanoseconds.
CREATE PERFETTO FUNCTION seconds(seconds INT) RETURNS INT AS
SELECT $seconds * 1000 * 1000 * 1000;

-- Converts a duration in minutes to nanoseconds, which is the default
-- representation of time durations in trace processor.
-- @arg minutes INT  Time duration in minutes.
-- @ret INT          Time duration in nanoseconds.
CREATE PERFETTO FUNCTION minutes(minutes INT) RETURNS INT AS
SELECT $minutes * 60 * 1000 * 1000 * 1000;

-- Converts a duration in hours to nanoseconds, which is the default
-- representation of time durations in trace processor.
-- @arg hours INT  Time duration in hours.
-- @ret INT        Time duration in nanoseconds.
CREATE PERFETTO FUNCTION hours(hours INT) RETURNS INT AS
SELECT $hours * 60 * 60 * 1000 * 1000 * 1000;

-- Converts a duration in days to nanoseconds, which is the default
-- representation of time durations in trace processor.
-- @arg days INT  Time duration in days.
-- @ret INT       Time duration in nanoseconds.
CREATE PERFETTO FUNCTION days(days INT) RETURNS INT AS
SELECT $days * 24 * 60 * 60 * 1000 * 1000 * 1000;
