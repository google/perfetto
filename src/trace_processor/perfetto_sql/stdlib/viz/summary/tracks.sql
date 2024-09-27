--
-- Copyright 2024 The Android Open Source Project
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

INCLUDE PERFETTO MODULE viz.summary.slices;

CREATE PERFETTO TABLE _thread_track_summary_by_utid_and_name AS
SELECT
  utid,
  parent_id,
  name,
  -- Only meaningful when track_count == 1.
  id as track_id,
  -- Only meaningful when track_count == 1.
  max_depth as max_depth,
  GROUP_CONCAT(id) AS track_ids,
  COUNT() AS track_count
FROM thread_track
JOIN _slice_track_summary USING (id)
GROUP BY utid, parent_id, name;

CREATE PERFETTO TABLE _process_track_summary_by_upid_and_parent_id_and_name AS
SELECT
  id,
  parent_id,
  upid,
  name,
  GROUP_CONCAT(id) AS track_ids,
  COUNT() AS track_count
FROM process_track
JOIN _slice_track_summary USING (id)
GROUP BY upid, parent_id, name;

CREATE PERFETTO TABLE _uid_track_track_summary_by_uid_and_name AS
SELECT
  uid,
  parent_id,
  name,
  GROUP_CONCAT(id) AS track_ids,
  COUNT() AS track_count
FROM uid_track
JOIN _slice_track_summary USING (id)
GROUP BY uid, parent_id, name;
