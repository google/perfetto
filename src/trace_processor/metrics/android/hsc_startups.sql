--
-- Copyright 2020 The Android Open Source Project
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

-- Must be invoked after populating launches table in android_startup.
CREATE VIEW frame_times AS
SELECT
    slices.ts AS ts,
    slices.ts + slices.dur AS ts_end,
    thread.name AS name,
    ROW_NUMBER() OVER(PARTITION BY thread.name ORDER BY ts ASC) as frame_number
FROM slices
INNER JOIN thread_track on slices.track_id = thread_track.id
INNER JOIN thread USING(utid)
WHERE slices.name="Choreographer#doFrame";

CREATE VIEW functions AS
SELECT
    slices.ts as ts,
    slices.dur as dur,
    thread.name as process_name,
    slices.name as function_name
FROM slices
INNER JOIN process_track on slices.track_id = process_track.id
INNER JOIN thread USING(upid);

CREATE TABLE hsc_based_startup_times(package STRING, id INT, dur_ns INT);

-- Netflix
INSERT INTO hsc_based_startup_times
SELECT
    launches.package as package,
    launches.id as id,
    frame_times.ts_end - launches.ts as ts_total
FROM frame_times
INNER JOIN launches on launches.package LIKE '%' || frame_times.name || '%'
WHERE frame_times.ts < (SELECT ts FROM functions WHERE function_name LIKE "animator%" AND process_name LIKE "%lix.mediaclient" ORDER BY ts LIMIT 1)
ORDER BY ts_total LIMIT 1;

-- Maps
INSERT INTO hsc_based_startup_times
SELECT
    launches.package as package,
    launches.id as id,
    frame_times.ts_end - launches.ts as ts_total
FROM frame_times
INNER JOIN launches on launches.package LIKE '%' || frame_times.name || '%'
WHERE frame_times.frame_number=1 AND frame_times.name LIKE "%maps%";
