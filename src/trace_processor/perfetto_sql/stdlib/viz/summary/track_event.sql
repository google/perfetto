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

INCLUDE PERFETTO MODULE viz.track_event_callstacks;

CREATE PERFETTO PIPELINE _track_event_tracks_unordered MATERIALIZED AS
SUBPIPELINE extracted AS (
  FROM track AS t
  |> WHERE t.type GLOB '*_track_event'
  |> SELECT
       t.id,
       t.name,
       t.parent_id,
       extract_arg(t.source_arg_set_id, 'child_ordering') AS ordering,
       extract_arg(t.source_arg_set_id, 'sibling_order_rank') AS rank,
       extract_arg(t.source_arg_set_id, 'description') AS description
)
FROM extracted AS t
|> LEFT JOIN extracted AS p ON t.parent_id = p.id
|> SELECT
     t.id,
     t.name,
     t.parent_id,
     p.ordering AS parent_ordering,
     coalesce(t.rank, 0) AS rank,
     t.description;

CREATE PERFETTO PIPELINE _min_ts_per_track MATERIALIZED AS
FROM counter
|> JOIN _track_event_tracks_unordered AS t ON counter.track_id = t.id
|> AGGREGATE min(ts) AS min_ts GROUP BY track_id
|> SELECT track_id AS id, min_ts
|> UNION ALL (
     FROM slice
     |> JOIN _track_event_tracks_unordered AS t ON slice.track_id = t.id
     |> AGGREGATE min(ts) AS min_ts GROUP BY track_id
     |> SELECT track_id AS id, min_ts
   );

CREATE PERFETTO PIPELINE _track_event_has_children MATERIALIZED AS
FROM track AS t
|> WHERE t.type GLOB '*_track_event' AND t.parent_id IS NOT NULL
|> SELECT DISTINCT t.parent_id AS id;

CREATE PERFETTO PIPELINE _track_event_tracks_ordered_groups MATERIALIZED AS
SUBPIPELINE explicit AS (
  FROM _track_event_tracks_unordered AS t
  |> WHERE t.parent_ordering = 'explicit'
  |> SELECT
       id,
       row_number() OVER (PARTITION BY parent_id ORDER BY rank) AS order_id
)
SUBPIPELINE chronological AS (
  FROM _track_event_tracks_unordered AS t
  |> LEFT JOIN _min_ts_per_track AS m USING (id)
  |> WHERE t.parent_ordering = 'chronological'
  |> SELECT
       t.id,
       row_number() OVER (PARTITION BY t.parent_id ORDER BY m.min_ts) AS order_id
)
FROM _track_event_tracks_unordered AS t
|> WHERE t.parent_ordering = 'lexicographic' OR t.parent_ordering IS NULL
|> SELECT
     id,
     row_number() OVER (PARTITION BY parent_id ORDER BY name) AS order_id
|> UNION ALL (FROM explicit |> SELECT id, order_id)
|> UNION ALL (FROM chronological |> SELECT id, order_id)
|> FORK AS unioned
|> JOIN track USING (id)
|> LEFT JOIN counter_track USING (id)
|> LEFT JOIN _track_event_has_children AS c USING (id)
|> LEFT JOIN _min_ts_per_track AS m USING (id)
|> LEFT JOIN _track_event_tracks_with_callstacks AS cs ON cs.track_id = unioned.id
|> AGGREGATE
     ANY_VALUE(extract_arg(track.dimension_arg_set_id, 'upid')) AS upid,
     ANY_VALUE(extract_arg(track.dimension_arg_set_id, 'utid')) AS utid,
     ANY_VALUE(track.parent_id) AS parent_id,
     ANY_VALUE(track.type GLOB '*counter*') AS is_counter,
     ANY_VALUE(track.name) AS name,
     min(extract_arg(track.source_arg_set_id, 'description')) AS description,
     min(counter_track.unit) AS unit,
     min(extract_arg(track.source_arg_set_id, 'builtin_counter_type')) AS builtin_counter_type,
     min(extract_arg(track.source_arg_set_id, 'y_axis_share_key')) AS y_axis_share_key,
     max(m.id IS NOT NULL) AS has_data,
     max(c.id IS NOT NULL) AS has_children,
     max(cs.track_id IS NOT NULL) AS has_callstacks,
     min(unioned.id) AS min_track_id,
     GROUP_CONCAT(unioned.id) AS track_ids,
     min(unioned.order_id) AS order_id
   GROUP BY track.track_group_id, coalesce(track.track_group_id, track.id)
|> ORDER BY parent_id, order_id;
