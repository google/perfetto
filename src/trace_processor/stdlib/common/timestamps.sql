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
SELECT CREATE_FUNCTION(
    'TRACE_START()',
    'LONG',
    'SELECT start_ts FROM trace_bounds;'
);

-- Fetch end of the trace.
-- @ret LONG  End of the trace in nanoseconds.
SELECT CREATE_FUNCTION(
    'TRACE_END()',
    'LONG',
    'SELECT end_ts FROM trace_bounds;'
);

-- Fetch duration of the trace.
-- @ret LONG  Duration of the trace in nanoseconds.
SELECT CREATE_FUNCTION(
    'TRACE_DUR()',
    'LONG',
    'SELECT TRACE_END() - TRACE_START();'
);

-- Checks whether two spans are overlapping.
--
-- @arg ts1 LONG      Start of first span.
-- @arg ts_end1 LONG  End of first span.
-- @arg ts2 LONG      Start of second span.
-- @arg ts_end2 LONG  End of second span.
-- @ret BOOL          Whether two spans are overlapping.
SELECT CREATE_FUNCTION(
  'IS_SPANS_OVERLAPPING(ts1 LONG, ts_end1 LONG, ts2 LONG, ts_end2 LONG)',
  'BOOL',
  '
    SELECT (IIF($ts1 < $ts2, $ts2, $ts1)
      < IIF($ts_end1 < $ts_end2, $ts_end1, $ts_end2))
  '
);

--Return the overlapping duration between two spans.
--If either duration is less than 0 or there's no intersection, 0 is returned
--
-- @arg ts1 LONG Timestamp of first slice start.
-- @arg dur1 LONG Duration of first slice.
-- @arg ts2 LONG Timestamp of second slice start.
-- @arg dur2 LONG Duration of second slice.
-- @ret INT               Overlapping duration
SELECT CREATE_FUNCTION(
  'SPANS_OVERLAPPING_DUR(ts1 LONG, dur1 LONG, ts2 LONG, dur2 LONG)',
  'INT',
  '
  SELECT
  CASE
    WHEN $dur1 = -1 OR $dur2 = -1 THEN 0
    WHEN $ts1 + $dur1 < $ts2 OR $ts2 + $dur2 < $ts1 THEN 0
    WHEN ($ts1 >= $ts2) AND ($ts1 + $dur1 <= $ts2 + $dur2) THEN $dur1
    WHEN ($ts1 < $ts2) AND ($ts1 + $dur1 < $ts2 + $dur2) THEN $ts1 + $dur1 - $ts2
    WHEN ($ts1 > $ts2) AND ($ts1 + $dur1 > $ts2 + $dur2) THEN $ts2 + $dur2 - $ts1
    ELSE $dur2
  END
  '
);
