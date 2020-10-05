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
CREATE VIEW functions AS
SELECT
    slices.ts as ts,
    slices.dur as dur,
    process.name as process_name,
    thread.name as thread_name,
    slices.name as function_name
FROM slices
INNER JOIN thread_track on slices.track_id = thread_track.id
INNER JOIN thread USING(utid)
INNER JOIN process USING(upid);

-- Animators don't occur on threads, so add them here.
CREATE VIEW animators AS
SELECT
    slices.ts AS ts,
    slices.dur AS dur,
    thread.name AS process_name,
    slices.name AS animator_name
FROM slices
INNER JOIN process_track on slices.track_id = process_track.id
INNER JOIN thread USING(upid)
WHERE slices.name LIKE "animator%";

CREATE VIEW frame_times AS
SELECT
    functions.ts AS ts,
    functions.ts + functions.dur AS ts_end,
    launches.package AS name,
    launches.id AS launch_id,
    ROW_NUMBER() OVER(PARTITION BY launches.id ORDER BY functions.ts ASC) as number
FROM functions
INNER JOIN launches on launches.package LIKE '%' || functions.process_name || '%'
WHERE functions.function_name="Choreographer#doFrame" AND functions.ts > launches.ts;

CREATE TABLE hsc_based_startup_times(package STRING, id INT, ts_total INT);

-- Calculator
INSERT INTO hsc_based_startup_times
SELECT
    launches.package as package,
    launches.id as id,
    frame_times.ts_end - launches.ts as ts_total
FROM frame_times
INNER JOIN launches on launches.package LIKE '%' || frame_times.name || '%'
WHERE frame_times.number=2 AND frame_times.name LIKE "%roid.calcul%" AND frame_times.launch_id = launches.id;

-- Calendar
INSERT INTO hsc_based_startup_times
SELECT
    launches.package as package,
    launches.id as id,
    frame_times.ts_end - launches.ts as ts_total
FROM frame_times
INNER JOIN launches on launches.package LIKE '%' || frame_times.name || '%'
WHERE frame_times.name LIKE "%id.calendar%" AND frame_times.launch_id = launches.id
ORDER BY ABS(frame_times.ts_end - (SELECT ts + dur FROM functions WHERE function_name LIKE "DrawFrame" AND process_name LIKE "%id.calendar" ORDER BY ts LIMIT 1)) LIMIT 1;

-- Camera
INSERT INTO hsc_based_startup_times
SELECT
    launches.package as package,
    launches.id as id,
    frame_times.ts_end - launches.ts as ts_total
FROM frame_times
INNER JOIN launches on launches.package LIKE '%' || frame_times.name || '%'
WHERE frame_times.number=2 AND frame_times.name LIKE "%GoogleCamera%" AND frame_times.launch_id = launches.id;

-- Chrome
INSERT INTO hsc_based_startup_times
SELECT
    launches.package as package,
    launches.id as id,
    frame_times.ts_end - launches.ts as ts_total
FROM frame_times
INNER JOIN launches on launches.package LIKE '%' || frame_times.name || '%'
WHERE frame_times.number=1 AND frame_times.name LIKE "%chrome%" AND frame_times.launch_id = launches.id;

-- Clock
INSERT INTO hsc_based_startup_times
SELECT
    launches.package as package,
    launches.id as id,
    frame_times.ts_end - launches.ts as ts_total
FROM frame_times
INNER JOIN launches on launches.package LIKE '%' || frame_times.name || '%'
WHERE frame_times.ts > (SELECT ts + dur FROM animators WHERE animator_name="animator:translationZ" AND process_name LIKE "%id.deskclock" ORDER BY ts DESC LIMIT 1) AND frame_times.name LIKE "%id.deskclock" AND frame_times.launch_id = launches.id
ORDER BY ts_total LIMIT 1;

-- Contacts
INSERT INTO hsc_based_startup_times
SELECT
    launches.package as package,
    launches.id as id,
    frame_times.ts_end - launches.ts as ts_total
FROM frame_times
INNER JOIN launches on launches.package LIKE '%' || frame_times.name || '%'
WHERE frame_times.number=3 AND frame_times.name LIKE "%id.contacts" AND frame_times.launch_id=launches.id;

-- Dialer
INSERT INTO hsc_based_startup_times
SELECT
    launches.package as package,
    launches.id as id,
    frame_times.ts_end - launches.ts as ts_total
FROM frame_times
INNER JOIN launches on launches.package LIKE '%' || frame_times.name || '%'
WHERE frame_times.number=1 AND frame_times.name LIKE "%id.dialer" AND frame_times.launch_id=launches.id;

-- Facebook
INSERT INTO hsc_based_startup_times
SELECT
    launches.package as package,
    launches.id as id,
    frame_times.ts_end - launches.ts as ts_total
FROM frame_times
INNER JOIN launches on launches.package LIKE '%' || frame_times.name || '%'
WHERE frame_times.ts > (SELECT ts+dur FROM slices WHERE slices.name LIKE "fb_startup_complete" ORDER BY ts LIMIT 1) AND frame_times.name LIKE "%ok.katana" AND frame_times.launch_id = launches.id
ORDER BY ts_total LIMIT 1;

-- Facebook Messenger
INSERT INTO hsc_based_startup_times
SELECT
    launches.package as package,
    launches.id as id,
    frame_times.ts_end - launches.ts as ts_total
FROM frame_times
INNER JOIN launches on launches.package LIKE '%' || frame_times.name || '%'
WHERE frame_times.ts > (SELECT ts+dur FROM slices WHERE slices.name LIKE "msgr_cold_start_to_cached_content" ORDER BY ts LIMIT 1) AND frame_times.name LIKE "%book.orca" AND frame_times.launch_id = launches.id
ORDER BY ts_total LIMIT 1;

-- Gmail
INSERT INTO hsc_based_startup_times
SELECT
    launches.package as package,
    launches.id as id,
    frame_times.ts_end - launches.ts as ts_total
FROM frame_times
INNER JOIN launches on launches.package LIKE '%' || frame_times.name || '%'
WHERE frame_times.ts > (SELECT ts + dur FROM animators WHERE animator_name="animator:elevation" AND process_name LIKE "%android.gm" ORDER BY ts DESC LIMIT 1) AND frame_times.name LIKE "%android.gm" AND frame_times.launch_id = launches.id
ORDER BY ts_total LIMIT 1;

-- Instagram
INSERT INTO hsc_based_startup_times
SELECT
    launches.package as package,
    launches.id as id,
    frame_times.ts_end - launches.ts as ts_total
FROM frame_times
INNER JOIN launches on launches.package LIKE '%' || frame_times.name || '%'
WHERE frame_times.ts > (SELECT ts+dur FROM slices WHERE slices.name LIKE "ig_cold_start_to_cached_content" ORDER BY ts LIMIT 1) AND frame_times.name LIKE "%gram.android" AND frame_times.launch_id = launches.id
ORDER BY ts_total LIMIT 1;

-- Maps
INSERT INTO hsc_based_startup_times
SELECT
    launches.package as package,
    launches.id as id,
    frame_times.ts_end - launches.ts as ts_total
FROM frame_times
INNER JOIN launches on launches.package LIKE '%' || frame_times.name || '%'
WHERE frame_times.number=1 AND frame_times.name LIKE "%maps%" AND frame_times.launch_id = launches.id;

-- Messages
INSERT INTO hsc_based_startup_times
SELECT
    launches.package as package,
    launches.id as id,
    frame_times.ts_end - launches.ts as ts_total
FROM frame_times
INNER JOIN launches on launches.package LIKE '%' || frame_times.name || '%'
WHERE frame_times.ts_end > (SELECT ts + dur FROM animators WHERE animator_name="animator:translationZ" AND process_name LIKE "%apps.messaging%" ORDER BY ts LIMIT 1) AND frame_times.name LIKE "%apps.messaging%" AND frame_times.launch_id = launches.id
ORDER BY ts_total LIMIT 1;

-- Netflix
INSERT INTO hsc_based_startup_times
SELECT
    launches.package as package,
    launches.id as id,
    frame_times.ts_end - launches.ts as ts_total
FROM frame_times
INNER JOIN launches on launches.package LIKE '%' || frame_times.name || '%'
WHERE frame_times.ts < (SELECT ts FROM animators WHERE animator_name LIKE "animator%" AND process_name LIKE "%lix.mediaclient" ORDER BY ts LIMIT 1) AND frame_times.name LIKE "%lix.mediaclient%" AND frame_times.launch_id = launches.id
ORDER BY ts_total DESC LIMIT 1;

-- Photos
INSERT INTO hsc_based_startup_times
SELECT
    launches.package as package,
    launches.id as id,
    frame_times.ts_end - launches.ts as ts_total
FROM frame_times
INNER JOIN launches on launches.package LIKE '%' || frame_times.name || '%'
WHERE frame_times.number=1 AND frame_times.name LIKE "%apps.photos%" AND frame_times.launch_id = launches.id;

-- Settings was deprecated in favor of reportFullyDrawn b/169694037.

-- Snapchat
INSERT INTO hsc_based_startup_times
SELECT
    launches.package as package,
    launches.id as id,
    frame_times.ts_end - launches.ts as ts_total
FROM frame_times
INNER JOIN launches on launches.package LIKE '%' || frame_times.name || '%'
WHERE frame_times.number=1 AND frame_times.name LIKE "%napchat.android" AND frame_times.launch_id = launches.id;

-- Twitter
INSERT INTO hsc_based_startup_times
SELECT
    launches.package as package,
    launches.id as id,
    frame_times.ts_end - launches.ts as ts_total
FROM frame_times
INNER JOIN launches on launches.package LIKE '%' || frame_times.name || '%'
WHERE frame_times.ts_end > (SELECT ts FROM animators WHERE animator_name="animator" AND process_name LIKE "%tter.android" ORDER BY ts LIMIT 1) AND frame_times.name LIKE "%tter.android" AND frame_times.launch_id = launches.id
ORDER BY ts_total LIMIT 1;

-- WhatsApp
INSERT INTO hsc_based_startup_times
SELECT
    launches.package as package,
    launches.id as id,
    frame_times.ts_end - launches.ts as ts_total
FROM frame_times
INNER JOIN launches on launches.package LIKE '%' || frame_times.name || '%'
WHERE frame_times.ts > (SELECT ts+dur FROM slices WHERE slices.name LIKE "wa_startup_complete" ORDER BY ts LIMIT 1) AND frame_times.name LIKE "%om.whatsapp" AND frame_times.launch_id = launches.id
ORDER BY ts_total LIMIT 1;

-- Youtube
INSERT INTO hsc_based_startup_times
SELECT
    launches.package as package,
    launches.id as id,
    frame_times.ts_end - launches.ts as ts_total
FROM frame_times
INNER JOIN launches on launches.package LIKE '%' || frame_times.name || '%'
WHERE frame_times.number=2 AND frame_times.name LIKE "%id.youtube" AND frame_times.launch_id = launches.id;
