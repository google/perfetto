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

CREATE PERFETTO VIEW _track_event_tracks_unordered AS
WITH extracted AS (
  SELECT
    t.id,
    t.parent_id,
    t.name,
    EXTRACT_ARG(t.source_arg_set_id, 'child_ordering') AS ordering,
    EXTRACT_ARG(t.source_arg_set_id, 'sibling_order_rank') AS rank
  FROM track t
)
SELECT
  t.id,
  t.parent_id,
  t.name,
  t.ordering,
  p.ordering AS parent_ordering,
  IFNULL(t.rank, 0) AS rank
FROM extracted t
LEFT JOIN extracted p ON t.parent_id = p.id
WHERE p.ordering IS NOT NULL;

CREATE PERFETTO TABLE _track_event_tracks_ordered AS
WITH lexicographic_and_none AS (
  SELECT
    id, parent_id, name,
    ROW_NUMBER() OVER (ORDER BY parent_id, name) AS order_id
  FROM _track_event_tracks_unordered
  WHERE parent_ordering = 'lexicographic'
),
explicit AS (
SELECT
  id, parent_id, name,
  ROW_NUMBER() OVER (ORDER BY parent_id, rank) AS order_id
FROM _track_event_tracks_unordered
WHERE parent_ordering = 'explicit'
),
slice_chronological AS (
  SELECT
    t.*,
    min(ts) AS min_ts
  FROM _track_event_tracks_unordered t
  JOIN slice s on t.id = s.track_id
  WHERE parent_ordering = 'chronological'
  GROUP BY track_id
),
counter_chronological AS (
  SELECT
    t.*,
    min(ts) AS min_ts
  FROM _track_event_tracks_unordered t
  JOIN counter s on t.id = s.track_id
  WHERE parent_ordering = 'chronological'
  GROUP BY track_id
),
slice_and_counter_chronological AS (
  SELECT t.*, u.min_ts
  FROM _track_event_tracks_unordered t
  LEFT JOIN (
    SELECT * FROM slice_chronological
    UNION ALL
    SELECT * FROM counter_chronological) u USING (id)
  WHERE t.parent_ordering = 'chronological'
),
chronological AS (
  SELECT
    id, parent_id, name,
    ROW_NUMBER() OVER (ORDER BY parent_id, min_ts) AS order_id
  FROM slice_and_counter_chronological
),
all_tracks AS (
  SELECT id, parent_id, name, order_id
  FROM lexicographic_and_none
  UNION
  SELECT id, parent_id, name, order_id
  FROM explicit
  UNION
  SELECT id, parent_id, name, order_id
  FROM chronological
)
SELECT id, order_id
FROM all_tracks all_t
ORDER BY parent_id, order_id;

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
LEFT JOIN _track_event_tracks_ordered USING (id)
GROUP BY utid, parent_id, order_id, name;

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
LEFT JOIN _track_event_tracks_ordered USING (id)
GROUP BY upid, parent_id, order_id, name;