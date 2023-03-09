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

SELECT IMPORT('common.counters');
SELECT IMPORT('common.timestamps');

SELECT CREATE_VIEW_FUNCTION(
    'INTERNAL_NUMBER_GENERATOR(to INT)',
    'num INT',
    'WITH NUMS AS
        (SELECT 1 num UNION SELECT num + 1
        from NUMS
        WHERE num < $to)
    SELECT num FROM NUMS;'
);

--
-- Get durations for percentile
--

-- All percentiles (range 1-100) for counter track ID in a given time range.
--
-- Percentiles are calculated by:
-- 1. Dividing the sum of duration in time range for each value in the counter
-- by duration of the counter in range. This gives us `percentile_for)value` (DOUBLE).
-- 2. Fetching each percentile by taking floor of each `percentile_for_value`, grouping by
-- resulting `percentile` and MIN from value for each grouping. As we are rounding down,
-- taking MIN assures most reliable data.
-- 3. Filling the possible gaps in percentiles by getting the minimal value from higher
-- percentiles for each gap.
--
-- @arg counter_track_id INT Id of the counter track.
-- @arg start_ts LONG        Timestamp of start of time range.
-- @arg end_ts LONG          Timestamp of end of time range.
-- @column percentile        All of the numbers from 1 to 100.
-- @column value             Value for the percentile.
SELECT CREATE_VIEW_FUNCTION(
    'COUNTER_PERCENTILES_FOR_TIME_RANGE(counter_track_id INT, start_ts LONG, end_ts LONG)',
    'percentile INT, value DOUBLE',
    'WITH percentiles_for_value AS (
        SELECT
            value,
            (CAST(SUM(dur) OVER(ORDER BY value ASC) AS DOUBLE) /
                ($end_ts - MAX($start_ts, EARLIEST_TIMESTAMP_FOR_COUNTER_TRACK($counter_track_id)))) * 100
            AS percentile_for_value
        FROM COUNTER_FOR_TIME_RANGE($counter_track_id, $start_ts, $end_ts)
        ORDER BY value ASC
    ),
    with_gaps AS (
        SELECT
            CAST(percentile_for_value AS INT) AS percentile,
            MIN(value) AS value
        FROM percentiles_for_value
        GROUP BY percentile
        ORDER BY percentile ASC)
    SELECT
        num AS percentile,
        IFNULL(value, MIN(value) OVER (ORDER BY percentile DESC)) AS value
    FROM INTERNAL_NUMBER_GENERATOR(100) AS nums
    LEFT JOIN with_gaps ON with_gaps.percentile = nums.num
    ORDER BY percentile DESC
    '
);

-- All percentiles (range 1-100) for counter track ID.
--
-- @arg counter_track_id INT Id of the counter track.
-- @column percentile        All of the numbers from 1 to 100.
-- @column value             Value for the percentile.
SELECT CREATE_VIEW_FUNCTION(
    'COUNTER_PERCENTILES_FOR_TRACK(counter_track_id INT)',
    'percentile INT, value DOUBLE',
    'SELECT * FROM COUNTER_PERCENTILES_FOR_TIME_RANGE($counter_track_id, TRACE_START(), TRACE_END());'
);

-- Value for specific percentile (range 1-100) for counter track ID in time range.
--
-- @arg counter_track_id INT Id of the counter track.
-- @arg percentile INT       Any of the numbers from 1 to 100.
-- @arg start_ts LONG        Timestamp of start of time range.
-- @arg end_ts LONG          Timestamp of end of time range.
-- @ret DOUBLE               Value for the percentile.
SELECT CREATE_FUNCTION(
    'COUNTER_TRACK_PERCENTILE_FOR_TIME(counter_track_id INT, percentile INT, start_ts LONG, end_ts LONG)',
    'DOUBLE',
    'SELECT value
    FROM COUNTER_PERCENTILES_FOR_TIME_RANGE($counter_track_id, $start_ts, $end_ts)
    WHERE percentile = $percentile;'
);

-- Value for specific percentile (range 1-100) for counter track ID.
--
-- @arg counter_track_id INT Id of the counter track.
-- @arg percentile INT       Any of the numbers from 1 to 100.
-- @ret DOUBLE               Value for the percentile.
SELECT CREATE_FUNCTION(
    'COUNTER_TRACK_PERCENTILE(counter_track_id INT, percentile INT)',
    'DOUBLE',
    'SELECT COUNTER_TRACK_PERCENTILE_FOR_TIME($counter_track_id, $percentile, TRACE_START(), TRACE_END());'
);

