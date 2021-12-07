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
DROP VIEW IF EXISTS functions;
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
DROP VIEW IF EXISTS animators;
CREATE VIEW animators AS
SELECT
    slices.ts AS ts,
    slices.dur AS dur,
    thread.name AS process_name,
    slices.name AS animator_name
FROM slices
INNER JOIN process_track on slices.track_id = process_track.id
INNER JOIN thread USING(upid)
WHERE slices.name GLOB "animator*";

DROP VIEW IF EXISTS frame_times;
CREATE VIEW frame_times AS
SELECT
    functions.ts AS ts,
    functions.ts + functions.dur AS ts_end,
    launches.package AS name,
    launches.id AS launch_id,
    ROW_NUMBER() OVER(PARTITION BY launches.id ORDER BY functions.ts ASC) as number
FROM functions
INNER JOIN launches on launches.package GLOB '*' || functions.process_name || '*'
WHERE functions.function_name GLOB "Choreographer#doFrame*" AND functions.ts > launches.ts;

DROP TABLE IF EXISTS hsc_based_startup_times;
CREATE TABLE hsc_based_startup_times(package STRING, id INT, ts_total INT);

-- Calculator
INSERT INTO hsc_based_startup_times
SELECT
    launches.package as package,
    launches.id as id,
    frame_times.ts_end - launches.ts as ts_total
FROM frame_times
INNER JOIN launches on launches.package GLOB '*' || frame_times.name || '*'
WHERE frame_times.number=2 AND frame_times.name GLOB "*roid.calcul*" AND frame_times.launch_id = launches.id;

-- Calendar
INSERT INTO hsc_based_startup_times
SELECT
    launches.package as package,
    launches.id as id,
    frame_times.ts_end - launches.ts as ts_total
FROM frame_times
INNER JOIN launches on launches.package GLOB '*' || frame_times.name || '*'
WHERE frame_times.name GLOB "*id.calendar*" AND frame_times.launch_id = launches.id
ORDER BY ABS(frame_times.ts_end - (SELECT ts + dur FROM functions WHERE function_name GLOB "DrawFrame*" AND process_name GLOB "*id.calendar" ORDER BY ts LIMIT 1)) LIMIT 1;

-- Camera
INSERT INTO hsc_based_startup_times
SELECT
    launches.package as package,
    launches.id as id,
    frame_times.ts_end - launches.ts as ts_total
FROM frame_times
INNER JOIN launches on launches.package GLOB '*' || frame_times.name || '*'
WHERE frame_times.number=2 AND frame_times.name GLOB "*GoogleCamera*" AND frame_times.launch_id = launches.id;

-- Chrome
INSERT INTO hsc_based_startup_times
SELECT
    launches.package as package,
    launches.id as id,
    frame_times.ts_end - launches.ts as ts_total
FROM frame_times
INNER JOIN launches on launches.package GLOB '*' || frame_times.name || '*'
WHERE frame_times.number=1 AND frame_times.name GLOB "*chrome*" AND frame_times.launch_id = launches.id;

-- Clock
INSERT INTO hsc_based_startup_times
SELECT
    launches.package as package,
    launches.id as id,
    frame_times.ts_end - launches.ts as ts_total
FROM frame_times
INNER JOIN launches on launches.package GLOB '*' || frame_times.name || '*'
WHERE frame_times.ts > (SELECT ts + dur FROM animators WHERE animator_name="animator:translationZ" AND process_name GLOB "*id.deskclock" ORDER BY (ts+dur) DESC LIMIT 1) AND frame_times.name GLOB "*id.deskclock" AND frame_times.launch_id = launches.id
ORDER BY ts_total LIMIT 1;

-- Contacts
INSERT INTO hsc_based_startup_times
SELECT
    launches.package as package,
    launches.id as id,
    frame_times.ts_end - launches.ts as ts_total
FROM frame_times
INNER JOIN launches on launches.package GLOB '*' || frame_times.name || '*'
WHERE frame_times.number=3 AND frame_times.name GLOB "*id.contacts" AND frame_times.launch_id=launches.id;

-- Dialer
INSERT INTO hsc_based_startup_times
SELECT
    launches.package as package,
    launches.id as id,
    frame_times.ts_end - launches.ts as ts_total
FROM frame_times
INNER JOIN launches on launches.package GLOB '*' || frame_times.name || '*'
WHERE frame_times.number=1 AND frame_times.name GLOB "*id.dialer" AND frame_times.launch_id=launches.id;

-- Facebook
INSERT INTO hsc_based_startup_times
SELECT
    launches.package as package,
    launches.id as id,
    frame_times.ts_end - launches.ts as ts_total
FROM frame_times
INNER JOIN launches on launches.package GLOB '*' || frame_times.name || '*'
WHERE frame_times.ts > (SELECT ts+dur FROM slices WHERE slices.name GLOB "fb_startup_complete" ORDER BY ts LIMIT 1) AND frame_times.name GLOB "*ok.katana" AND frame_times.launch_id = launches.id
ORDER BY ts_total LIMIT 1;

-- Facebook Messenger
INSERT INTO hsc_based_startup_times
SELECT
    launches.package as package,
    launches.id as id,
    frame_times.ts_end - launches.ts as ts_total
FROM frame_times
INNER JOIN launches on launches.package GLOB '*' || frame_times.name || '*'
WHERE frame_times.ts > (SELECT ts+dur FROM slices WHERE slices.name GLOB "msgr_cold_start_to_cached_content" ORDER BY ts LIMIT 1) AND frame_times.name GLOB "*book.orca" AND frame_times.launch_id = launches.id
ORDER BY ts_total LIMIT 1;

-- Gmail
INSERT INTO hsc_based_startup_times
SELECT
    launches.package as package,
    launches.id as id,
    frame_times.ts_end - launches.ts as ts_total
FROM frame_times
INNER JOIN launches on launches.package GLOB '*' || frame_times.name || '*'
WHERE frame_times.ts > (SELECT ts + dur FROM animators WHERE animator_name="animator:elevation" AND process_name GLOB "*android.gm" ORDER BY (ts+dur) DESC LIMIT 1) AND frame_times.name GLOB "*android.gm" AND frame_times.launch_id = launches.id
ORDER BY ts_total LIMIT 1;

-- Instagram
INSERT INTO hsc_based_startup_times
SELECT
    launches.package as package,
    launches.id as id,
    frame_times.ts_end - launches.ts as ts_total
FROM frame_times
INNER JOIN launches on launches.package GLOB '*' || frame_times.name || '*'
WHERE frame_times.ts > (SELECT ts+dur FROM slices WHERE slices.name GLOB "ig_cold_start_to_cached_content" ORDER BY ts LIMIT 1) AND frame_times.name GLOB "*gram.android" AND frame_times.launch_id = launches.id
ORDER BY ts_total LIMIT 1;

-- Maps
INSERT INTO hsc_based_startup_times
SELECT
    launches.package as package,
    launches.id as id,
    frame_times.ts_end - launches.ts as ts_total
FROM frame_times
INNER JOIN launches on launches.package GLOB '*' || frame_times.name || '*'
WHERE frame_times.number=1 AND frame_times.name GLOB "*maps*" AND frame_times.launch_id = launches.id;

-- Messages
INSERT INTO hsc_based_startup_times
SELECT
    launches.package as package,
    launches.id as id,
    frame_times.ts_end - launches.ts as ts_total
FROM frame_times
INNER JOIN launches on launches.package GLOB '*' || frame_times.name || '*'
WHERE frame_times.ts_end > (SELECT ts + dur FROM animators WHERE animator_name="animator:translationZ" AND process_name GLOB "*apps.messaging*" ORDER BY ts LIMIT 1) AND frame_times.name GLOB "*apps.messaging*" AND frame_times.launch_id = launches.id
ORDER BY ts_total LIMIT 1;

-- Netflix
INSERT INTO hsc_based_startup_times
SELECT
    launches.package as package,
    launches.id as id,
    frame_times.ts_end - launches.ts as ts_total
FROM frame_times
INNER JOIN launches on launches.package GLOB '*' || frame_times.name || '*'
WHERE frame_times.ts < (SELECT ts FROM animators WHERE animator_name GLOB "animator*" AND process_name GLOB "*lix.mediaclient" ORDER BY ts LIMIT 1) AND frame_times.name GLOB "*lix.mediaclient*" AND frame_times.launch_id = launches.id
ORDER BY ts_total DESC LIMIT 1;

-- Photos
INSERT INTO hsc_based_startup_times
SELECT
    launches.package as package,
    launches.id as id,
    frame_times.ts_end - launches.ts as ts_total
FROM frame_times
INNER JOIN launches on launches.package GLOB '*' || frame_times.name || '*'
WHERE frame_times.number=1 AND frame_times.name GLOB "*apps.photos*" AND frame_times.launch_id = launches.id;

-- Settings was deprecated in favor of reportFullyDrawn b/169694037.

-- Snapchat
INSERT INTO hsc_based_startup_times
SELECT
    launches.package as package,
    launches.id as id,
    frame_times.ts_end - launches.ts as ts_total
FROM frame_times
INNER JOIN launches on launches.package GLOB '*' || frame_times.name || '*'
WHERE frame_times.number=1 AND frame_times.name GLOB "*napchat.android" AND frame_times.launch_id = launches.id;

-- Twitter
INSERT INTO hsc_based_startup_times
SELECT
    launches.package as package,
    launches.id as id,
    frame_times.ts_end - launches.ts as ts_total
FROM frame_times
INNER JOIN launches on launches.package GLOB '*' || frame_times.name || '*'
WHERE frame_times.ts_end > (SELECT ts FROM animators WHERE animator_name="animator" AND process_name GLOB "*tter.android" ORDER BY ts LIMIT 1) AND frame_times.name GLOB "*tter.android" AND frame_times.launch_id = launches.id
ORDER BY ts_total LIMIT 1;

-- WhatsApp
INSERT INTO hsc_based_startup_times
SELECT
    launches.package as package,
    launches.id as id,
    frame_times.ts_end - launches.ts as ts_total
FROM frame_times
INNER JOIN launches on launches.package GLOB '*' || frame_times.name || '*'
WHERE frame_times.ts > (SELECT ts+dur FROM slices WHERE slices.name GLOB "wa_startup_complete" ORDER BY ts LIMIT 1) AND frame_times.name GLOB "*om.whatsapp" AND frame_times.launch_id = launches.id
ORDER BY ts_total LIMIT 1;

-- Youtube
INSERT INTO hsc_based_startup_times
SELECT
    launches.package as package,
    launches.id as id,
    frame_times.ts_end - launches.ts as ts_total
FROM frame_times
INNER JOIN launches on launches.package GLOB '*' || frame_times.name || '*'
WHERE frame_times.number=2 AND frame_times.name GLOB "*id.youtube" AND frame_times.launch_id = launches.id;
