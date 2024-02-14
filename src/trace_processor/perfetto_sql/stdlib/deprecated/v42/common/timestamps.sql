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

INCLUDE PERFETTO MODULE time.conversion;

CREATE PERFETTO FUNCTION is_spans_overlapping(
  ts1 LONG,
  ts_end1 LONG,
  ts2 LONG,
  ts_end2 LONG)
RETURNS BOOL AS
SELECT (IIF($ts1 < $ts2, $ts2, $ts1)
      < IIF($ts_end1 < $ts_end2, $ts_end1, $ts_end2));

CREATE PERFETTO FUNCTION spans_overlapping_dur(
  ts1 LONG,
  dur1 LONG,
  ts2 LONG,
  dur2 LONG
)
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

-- Renames

CREATE PERFETTO FUNCTION ns(nanos INT)
RETURNS INT AS
SELECT time_from_ns($nanos);

CREATE PERFETTO FUNCTION us(micros INT)
RETURNS INT AS
SELECT time_from_us($micros);

CREATE PERFETTO FUNCTION ms(millis INT)
RETURNS INT AS
SELECT time_from_ms($millis);

CREATE PERFETTO FUNCTION seconds(seconds INT)
RETURNS INT AS
SELECT time_from_s($seconds);

CREATE PERFETTO FUNCTION minutes(minutes INT)
RETURNS INT AS
SELECT time_from_min($minutes);

CREATE PERFETTO FUNCTION hours(hours INT)
RETURNS INT AS
SELECT time_from_hours($hours);

CREATE PERFETTO FUNCTION days(days INT)
RETURNS INT AS
SELECT time_from_days($days);
