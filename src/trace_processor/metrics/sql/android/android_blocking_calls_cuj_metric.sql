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

-- Create the base table (`android_jank_cuj`) containing all completed CUJs
-- found in the trace.
-- This script will use the `android_jank_cuj_main_thread_cuj_boundary`,
-- containing bounds of jank CUJs.
SELECT RUN_METRIC('android/android_jank_cuj.sql');

INCLUDE PERFETTO MODULE android.slices;
INCLUDE PERFETTO MODULE android.binder;
INCLUDE PERFETTO MODULE android.critical_blocking_calls;

-- Jank "J<*>" and latency "L<*>" cujs are put together in android_cujs table.
-- They are computed separately as latency ones are slightly different, don't
-- currently have the same way to be cancelled, and are not anchored to vsyncs.
DROP TABLE IF EXISTS android_cujs;
CREATE TABLE android_cujs AS
WITH latency_cujs AS (
    SELECT
        ROW_NUMBER() OVER (ORDER BY ts) AS cuj_id,
        process.upid AS upid,
        -- Latency CUJs don't have a well defined thread. Let's always consider
        -- the app main thread for those.
        process.upid AS utid,
        process.name AS process_name,
        process_metadata.metadata AS process_metadata,
        -- Extracts "CUJ_NAME" from "L<CUJ_NAME>"
        SUBSTR(slice.name, 3, LENGTH(slice.name) - 3) AS cuj_name,
        ts,
        dur,
        ts + dur AS ts_end,
        'completed' AS state
    FROM slice
        JOIN process_track
          ON slice.track_id = process_track.id
        JOIN process USING (upid)
        JOIN process_metadata USING (upid)
    WHERE
        slice.name GLOB 'L<*>'
    AND dur > 0
),
all_cujs AS (
    SELECT
        cuj_id,
        cuj.upid,
        t.utid as ui_thread,
        process_name,
        process_metadata,
        cuj_name,
        tb.ts,
        tb.dur,
        tb.ts_end
    FROM android_jank_cuj_main_thread_cuj_boundary tb
        JOIN android_jank_cuj cuj USING (cuj_id)
        JOIN android_jank_cuj_main_thread t USING (cuj_id)
UNION
    SELECT
        cuj_id,
        upid,
        utid as ui_thread,
        process_name,
        process_metadata,
        cuj_name,
        ts,
        dur,
        ts_end
    FROM latency_cujs
)
SELECT ROW_NUMBER() OVER (ORDER BY ts) AS cuj_id, *
FROM all_cujs;

-- We have:
--  (1) a list of slices from the main thread of each process from the
--  all_main_thread_relevant_slices table.
--  (2) a list of android cuj with beginning, end, and process
-- It's needed to:
--  (1) assign a cuj to each slice. If there are multiple cujs going on during a
--      slice, there needs to be 2 entries for that slice, one for each cuj id.
--  (2) each slice needs to be trimmed to be fully inside the cuj associated
--      (as we don't care about what's outside cujs)
DROP TABLE IF EXISTS main_thread_slices_scoped_to_cujs;
CREATE PERFETTO TABLE main_thread_slices_scoped_to_cujs AS
SELECT
    s.id,
    s.id AS slice_id,
    s.name,
    max(s.ts, cuj.ts) AS ts,
    min(s.ts + s.dur, cuj.ts_end) as ts_end,
    min(s.ts + s.dur, cuj.ts_end) - max(s.ts, cuj.ts) AS dur,
    cuj.cuj_id,
    cuj.cuj_name,
    s.process_name,
    s.upid,
    s.utid
FROM _android_critical_blocking_calls s
    JOIN  android_cujs cuj
    -- only when there is an overlap
    ON s.ts + s.dur > cuj.ts AND s.ts < cuj.ts_end
        -- and are from the same process
        AND s.upid = cuj.upid
        -- from the CUJ ui thread only
        AND s.utid = cuj.ui_thread;


DROP TABLE IF EXISTS android_blocking_calls_cuj_calls;
CREATE TABLE android_blocking_calls_cuj_calls AS
SELECT
    name,
    COUNT(*) AS occurrences,
    MAX(dur) AS max_dur_ns,
    MIN(dur) AS min_dur_ns,
    SUM(dur) AS total_dur_ns,
    upid,
    cuj_id,
    cuj_name,
    process_name
FROM
    main_thread_slices_scoped_to_cujs
GROUP BY name, upid, cuj_id, cuj_name, process_name
ORDER BY cuj_id;


DROP VIEW IF EXISTS android_blocking_calls_cuj_metric_output;
CREATE PERFETTO VIEW android_blocking_calls_cuj_metric_output AS
SELECT AndroidBlockingCallsCujMetric('cuj', (
    SELECT RepeatedField(
        AndroidBlockingCallsCujMetric_Cuj(
            'id', cuj_id,
            'name', cuj_name,
            'process', process_metadata,
            'ts',  cuj.ts,
            'dur', cuj.dur,
            'blocking_calls', (
                SELECT RepeatedField(
                    AndroidBlockingCall(
                        'name', b.name,
                        'cnt', b.occurrences,
                        'total_dur_ms', CAST(total_dur_ns / 1e6 AS INT),
                        'max_dur_ms', CAST(max_dur_ns / 1e6 AS INT),
                        'min_dur_ms', CAST(min_dur_ns / 1e6 AS INT),
                        'total_dur_ns', b.total_dur_ns,
                        'max_dur_ns', b.max_dur_ns,
                        'min_dur_ns', b.min_dur_ns
                    )
                )
                FROM android_blocking_calls_cuj_calls b
                WHERE b.cuj_id = cuj.cuj_id and b.upid = cuj.upid
                ORDER BY total_dur_ns DESC
            )
        )
    )
    FROM android_cujs cuj
    ORDER BY cuj.cuj_id ASC
));
