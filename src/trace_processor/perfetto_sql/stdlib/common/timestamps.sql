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
CREATE PERFETTO FUNCTION trace_start()
-- Start of the trace in nanoseconds.
RETURNS LONG AS
SELECT start_ts FROM trace_bounds;

-- Fetch end of the trace.
CREATE PERFETTO FUNCTION trace_end()
-- End of the trace in nanoseconds.
RETURNS LONG AS
SELECT end_ts FROM trace_bounds;

-- Fetch duration of the trace.
CREATE PERFETTO FUNCTION trace_dur()
-- Duration of the trace in nanoseconds.
RETURNS LONG AS
SELECT trace_end() - trace_start();

-- Checks whether two spans are overlapping.
CREATE PERFETTO FUNCTION is_spans_overlapping(
  -- Start of first span.
  ts1 LONG,
  -- End of first span.
  ts_end1 LONG,
  -- Start of second span.
  ts2 LONG,
  -- End of second span.
  ts_end2 LONG)
-- Whether two spans are overlapping.
RETURNS BOOL AS
SELECT (IIF($ts1 < $ts2, $ts2, $ts1)
      < IIF($ts_end1 < $ts_end2, $ts_end1, $ts_end2));

--Return the overlapping duration between two spans.
--If either duration is less than 0 or there's no intersection, 0 is returned
CREATE PERFETTO FUNCTION spans_overlapping_dur(
  -- Timestamp of first slice start.
  ts1 LONG,
  -- Duration of first slice.
  dur1 LONG,
  -- Timestamp of second slice start.
  ts2 LONG,
  -- Duration of second slice.
  dur2 LONG
)
-- Overlapping duration
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
CREATE PERFETTO FUNCTION ns(
  -- Time duration in nanoseconds.
  nanos INT
)
-- Time duration in nanoseconds.
RETURNS INT AS
SELECT $nanos;

-- Converts a duration in microseconds to nanoseconds, which is the default
-- representation of time durations in trace processor.
CREATE PERFETTO FUNCTION us(
  -- Time duration in microseconds.
  micros INT
)
-- Time duration in nanoseconds.
RETURNS INT AS
SELECT $micros * 1000;

-- Converts a duration in millseconds to nanoseconds, which is the default
-- representation of time durations in trace processor.
CREATE PERFETTO FUNCTION ms(
  -- Time duration in milliseconds.
  millis INT
)
-- Time duration in nanoseconds.
RETURNS INT AS
SELECT $millis * 1000 * 1000;

-- Converts a duration in seconds to nanoseconds, which is the default
-- representation of time durations in trace processor.
CREATE PERFETTO FUNCTION seconds(
  -- Time duration in seconds.
  seconds INT
)
-- Time duration in nanoseconds.
RETURNS INT AS
SELECT $seconds * 1000 * 1000 * 1000;

-- Converts a duration in minutes to nanoseconds, which is the default
-- representation of time durations in trace processor.
CREATE PERFETTO FUNCTION minutes(
  -- Time duration in minutes.
  minutes INT
)
-- Time duration in nanoseconds.
RETURNS INT AS
SELECT $minutes * 60 * 1000 * 1000 * 1000;

-- Converts a duration in hours to nanoseconds, which is the default
-- representation of time durations in trace processor.
CREATE PERFETTO FUNCTION hours(
  -- Time duration in hours.
  hours INT
)
-- Time duration in nanoseconds.
RETURNS INT AS
SELECT $hours * 60 * 60 * 1000 * 1000 * 1000;

-- Converts a duration in days to nanoseconds, which is the default
-- representation of time durations in trace processor.
CREATE PERFETTO FUNCTION days(
-- @arg days INT  Time duration in days.
  days INT
)
-- Time duration in nanoseconds.
RETURNS INT AS
SELECT $days * 24 * 60 * 60 * 1000 * 1000 * 1000;
