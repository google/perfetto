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

-- Needed for the scroll_jank table to tell which updates were janky.
SELECT RUN_METRIC('chrome/scroll_jank.sql');

-- Causes of jank which we will join together. The process to add new causes
-- should be pretty straight forward.
--
-- 1) Determine a query (or sequence of queries) which identifies a
--    InputLatency::GestureScrollUpdate (which can be found in the scroll_jank
--    table) that was effected by the cause you are investigating.
-- 2) output the InputLatency::GestureScrollUpdate id from the slice table (or
--    scroll_jank table), along with a "true" ish value if that
--    InputLatency::GestureScrollUpdate was affected by your cause
-- 3) Add your new metric file in the SELECT RUN_METRIC lines below.
-- 4) Add a LEFT JOIN on your output table joining on the
--    InputLatency::GestureScrollUpdate id with scroll_jank_cause_joined.
-- 5) modify the scroll_jank_cause_explained_jank to include your cause.
SELECT RUN_METRIC('chrome/scroll_jank_cause_blocking_touch_move.sql');
SELECT RUN_METRIC('chrome/scroll_jank_cause_blocking_task.sql');
SELECT RUN_METRIC('chrome/scroll_jank_cause_get_bitmap.sql');

DROP VIEW IF EXISTS scroll_jank_cause_joined;
CREATE PERFETTO VIEW scroll_jank_cause_joined AS
SELECT
  COALESCE(move.blocking_touch_move, 0) AS blocking_touch_move,
  COALESCE(task.blocked_by_language_detection, 0)
  AS blocked_by_language_detection,
  COALESCE(task.blocked_by_copy_request, 0) AS blocked_by_copy_request,
  COALESCE(bitmap.blocked_by_bitmap, 0) AS blocked_by_bitmap,
  COALESCE(bitmap.blocked_by_toolbar, 0) AS blocked_by_toolbar,
  COALESCE(bitmap.blocked_by_bitmap_no_toolbar, 0)
  AS blocked_by_bitmap_no_toolbar,
  jank.*
FROM
  scroll_jank jank LEFT JOIN
  scroll_jank_cause_blocking_touch_move move
  ON jank.id = move.scroll_id LEFT JOIN
  scroll_jank_cause_blocking_task task
  ON jank.id = task.scroll_id LEFT JOIN
  scroll_jank_cause_get_bitmap bitmap
  ON jank.id = bitmap.scroll_id;

DROP VIEW IF EXISTS scroll_jank_cause_explained_jank;
CREATE PERFETTO VIEW scroll_jank_cause_explained_jank AS
SELECT
  CASE WHEN
      NOT jank
      THEN
      FALSE
    ELSE
      COALESCE(blocking_touch_move
        OR blocked_by_language_detection
        OR blocked_by_copy_request
        OR blocked_by_bitmap, FALSE)
  END AS explained_jank,
  jank.*
FROM scroll_jank_cause_joined jank;

DROP VIEW IF EXISTS scroll_jank_cause;
CREATE PERFETTO VIEW scroll_jank_cause AS
SELECT
  jank AND NOT explained_jank AS unexplained_jank,
  jank.*
FROM scroll_jank_cause_explained_jank jank;
