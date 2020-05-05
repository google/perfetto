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
    launches.package AS name,
    launches.id AS launch_id,
    ROW_NUMBER() OVER(PARTITION BY launches.id ORDER BY slices.ts ASC) as frame_number
FROM slices
INNER JOIN thread_track on slices.track_id = thread_track.id
INNER JOIN thread USING(utid)
INNER JOIN launches on launches.package LIKE '%' || thread.name || '%'
WHERE slices.name="Choreographer#doFrame" and slices.ts > launches.ts;

CREATE VIEW functions AS
SELECT
    slices.ts as ts,
    slices.dur as dur,
    thread.name as process_name,
    slices.name as function_name
FROM slices
INNER JOIN process_track on slices.track_id = process_track.id
INNER JOIN thread USING(upid);

CREATE TABLE hsc_based_startup_times(package STRING, id INT, ts_total INT);

-- Calculator
INSERT INTO hsc_based_startup_times
SELECT
    launches.package as package,
    launches.id as id,
    frame_times.ts_end - launches.ts as ts_total
FROM frame_times
INNER JOIN launches on launches.package LIKE '%' || frame_times.name || '%'
WHERE frame_times.frame_number=2 AND frame_times.name LIKE "%roid.calcul%" AND frame_times.launch_id = launches.id;

-- Calendar
INSERT INTO hsc_based_startup_times
SELECT
    launches.package as package,
    launches.id as id,
    frame_times.ts_end - launches.ts as ts_total
FROM frame_times
INNER JOIN launches on launches.package LIKE '%' || frame_times.name || '%'
WHERE frame_times.ts_end > (SELECT ts + dur FROM functions WHERE function_name LIKE "animator:growScale" AND process_name LIKE "%id.calendar" ORDER BY ts DESC LIMIT 1) AND frame_times.name LIKE "%id.calendar%" AND frame_times.launch_id = launches.id
ORDER BY ts_total LIMIT 1;

-- Camera
INSERT INTO hsc_based_startup_times
SELECT
    launches.package as package,
    launches.id as id,
    frame_times.ts_end - launches.ts as ts_total
FROM frame_times
INNER JOIN launches on launches.package LIKE '%' || frame_times.name || '%'
WHERE frame_times.ts > (SELECT ts + dur FROM functions WHERE function_name="ShutterButtonEnabled" AND process_name LIKE "%id.GoogleCamera%" ORDER BY ts LIMIT 1) AND frame_times.name LIKE "%id.GoogleCamera%" AND frame_times.launch_id = launches.id
ORDER BY ts_total LIMIT 1;

-- Chrome
INSERT INTO hsc_based_startup_times
SELECT
    launches.package as package,
    launches.id as id,
    frame_times.ts_end - launches.ts as ts_total
FROM frame_times
INNER JOIN launches on launches.package LIKE '%' || frame_times.name || '%'
WHERE frame_times.frame_number=1 AND frame_times.name LIKE "%chrome%" AND frame_times.launch_id = launches.id;

-- Clock
INSERT INTO hsc_based_startup_times
SELECT
    launches.package as package,
    launches.id as id,
    frame_times.ts_end - launches.ts as ts_total
FROM frame_times
INNER JOIN launches on launches.package LIKE '%' || frame_times.name || '%'
WHERE frame_times.ts > (SELECT ts + dur FROM functions WHERE function_name="animator:translationZ" AND process_name LIKE "%id.deskclock" ORDER BY ts DESC LIMIT 1) AND frame_times.name LIKE "%id.deskclock" AND frame_times.launch_id = launches.id
ORDER BY ts_total LIMIT 1;

-- Contacts
INSERT INTO hsc_based_startup_times
SELECT
    launches.package as package,
    launches.id as id,
    frame_times.ts_end - launches.ts as ts_total
FROM frame_times
INNER JOIN launches on launches.package LIKE '%' || frame_times.name || '%'
WHERE frame_times.ts > (SELECT ts + dur FROM functions WHERE function_name="animator:elevation" AND process_name LIKE "%id.contacts" ORDER BY ts DESC LIMIT 1) AND frame_times.name LIKE "%id.contacts" AND frame_times.launch_id = launches.id
ORDER BY ts_total LIMIT 1;

-- Dialer
INSERT INTO hsc_based_startup_times
SELECT
    launches.package as package,
    launches.id as id,
    frame_times.ts_end - launches.ts as ts_total
FROM frame_times
INNER JOIN launches on launches.package LIKE '%' || frame_times.name || '%'
WHERE frame_times.frame_number=2 AND frame_times.name LIKE "%id.dialer" AND frame_times.launch_id = launches.id;

-- Gmail
INSERT INTO hsc_based_startup_times
SELECT
    launches.package as package,
    launches.id as id,
    frame_times.ts_end - launches.ts as ts_total
FROM frame_times
INNER JOIN launches on launches.package LIKE '%' || frame_times.name || '%'
WHERE frame_times.ts > (SELECT ts + dur FROM functions WHERE function_name="animator:elevation" AND process_name LIKE "%android.gm" ORDER BY ts DESC LIMIT 1) AND frame_times.name LIKE "%android.gm" AND frame_times.launch_id = launches.id
ORDER BY ts_total LIMIT 1;

-- Instagram
INSERT INTO hsc_based_startup_times
SELECT
    package as package,
    id as id,
    (SELECT ts + dur FROM slices WHERE slices.name LIKE "Start proc%mqtt" ORDER BY ts LIMIT 1) - launches.ts as ts_total
FROM launches
WHERE launches.package="com.instagram.android";

-- Maps
INSERT INTO hsc_based_startup_times
SELECT
    launches.package as package,
    launches.id as id,
    frame_times.ts_end - launches.ts as ts_total
FROM frame_times
INNER JOIN launches on launches.package LIKE '%' || frame_times.name || '%'
WHERE frame_times.frame_number=1 AND frame_times.name LIKE "%maps%" AND frame_times.launch_id = launches.id;

-- Messages
INSERT INTO hsc_based_startup_times
SELECT
    launches.package as package,
    launches.id as id,
    frame_times.ts_end - launches.ts as ts_total
FROM frame_times
INNER JOIN launches on launches.package LIKE '%' || frame_times.name || '%'
WHERE frame_times.ts_end > (SELECT ts + dur FROM functions WHERE function_name="animator:translationZ" AND process_name LIKE "%apps.messaging%" ORDER BY ts DESC LIMIT 1) AND frame_times.name LIKE "%apps.messaging%" AND frame_times.launch_id = launches.id
ORDER BY ts_total LIMIT 1;

-- Netflix
INSERT INTO hsc_based_startup_times
SELECT
    launches.package as package,
    launches.id as id,
    frame_times.ts_end - launches.ts as ts_total
FROM frame_times
INNER JOIN launches on launches.package LIKE '%' || frame_times.name || '%'
WHERE frame_times.ts < (SELECT ts FROM functions WHERE function_name LIKE "animator%" AND process_name LIKE "%lix.mediaclient" ORDER BY ts LIMIT 1) AND frame_times.name LIKE "%lix.mediaclient%" AND frame_times.launch_id = launches.id
ORDER BY ts_total DESC LIMIT 1;

-- Photos
INSERT INTO hsc_based_startup_times
SELECT
    launches.package as package,
    launches.id as id,
    frame_times.ts_end - launches.ts as ts_total
FROM frame_times
INNER JOIN launches on launches.package LIKE '%' || frame_times.name || '%'
WHERE frame_times.frame_number=1 AND frame_times.name LIKE "%apps.photos%" AND frame_times.launch_id = launches.id;

-- Settings
INSERT INTO hsc_based_startup_times
SELECT
    launches.package as package,
    launches.id as id,
    frame_times.ts_end - launches.ts as ts_total
FROM frame_times
INNER JOIN launches on launches.package LIKE '%' || frame_times.name || '%'
WHERE frame_times.frame_number=4 AND frame_times.name LIKE "%settings%" AND frame_times.launch_id = launches.id;

-- Snapchat
INSERT INTO hsc_based_startup_times
SELECT
    launches.package as package,
    launches.id as id,
    frame_times.ts_end - launches.ts as ts_total
FROM frame_times
INNER JOIN launches on launches.package LIKE '%' || frame_times.name || '%'
WHERE frame_times.frame_number=1 AND frame_times.name LIKE "%napchat.android" AND frame_times.launch_id = launches.id;

-- Twitter
INSERT INTO hsc_based_startup_times
SELECT
    launches.package as package,
    launches.id as id,
    frame_times.ts_end - launches.ts as ts_total
FROM frame_times
INNER JOIN launches on launches.package LIKE '%' || frame_times.name || '%'
WHERE frame_times.ts > (SELECT ts + dur FROM functions WHERE function_name="animator:translationZ" AND process_name LIKE "%tter.android" ORDER BY ts DESC LIMIT 1) AND frame_times.name LIKE "%tter.android" AND frame_times.launch_id = launches.id
ORDER BY ts_total LIMIT 1;

-- Youtube
INSERT INTO hsc_based_startup_times
SELECT
    launches.package as package,
    launches.id as id,
    frame_times.ts_end - launches.ts as ts_total
FROM frame_times
INNER JOIN launches on launches.package LIKE '%' || frame_times.name || '%'
WHERE frame_times.frame_number=1 AND frame_times.name LIKE "%id.youtube" AND frame_times.launch_id = launches.id;
