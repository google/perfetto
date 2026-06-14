--
-- Copyright 2019 The Android Open Source Project
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

CREATE PERFETTO PIPELINE _startups_maxsdk28 MATERIALIZED AS
-- Warm and cold starts only are based on the launching slice.
SUBPIPELINE warm_and_cold AS (
  FROM _startup_events AS le
  |> SELECT
       le.ts,
       le.ts_end AS ts_end,
       le.ts AS ts_begin,
       le.ts_end - le.ts AS dur,
       le.package_name AS package,
       NULL AS startup_type
)
-- Hot starts don't have a launching slice so we use activityResume as a
-- proxy. This implementation will also count warm and cold starts but they
-- are removed via the temporal antijoin against the warm/cold coverage.
SUBPIPELINE maybe_hot AS (
  FROM thread_slice AS sl
  |> JOIN android_first_frame_after(sl.ts) AS rs
  |> WHERE sl.name = 'activityResume' AND sl.is_main_thread
  |> INTERVAL DROP IF COVERING BEGIN (FROM warm_and_cold |> SELECT ts_begin AS ts, dur)
  |> SELECT
       sl.ts,
       rs.ts + rs.dur AS ts_end,
       sl.ts AS ts_begin,
       (rs.ts + rs.dur) - sl.ts AS dur,
       coalesce(sl.process_name, sl.thread_name, 'unknown') AS package,
       "hot" AS startup_type
)
FROM warm_and_cold
|> UNION ALL (FROM maybe_hot)
|> SELECT ts, ts_end, dur, package, startup_type
|> ORDER BY ts;
