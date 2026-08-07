--
-- Copyright 2026 The Android Open Source Project
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

-- Maps GPU render-stage slices projected onto authored TrackEvent queues back
-- to their canonical gpu_slice rows. Internal only for now.
CREATE PERFETTO TABLE _gpu_render_stage_projections(
  -- The slice projected onto an authored TrackEvent queue.
  projected_slice_id ID(slice.id),
  -- The canonical GPU render-stage slice.
  canonical_slice_id JOINID(slice.id),
  -- The process owning the projected queue, or NULL for a global queue.
  upid JOINID(process.id)
)
AS
SELECT
  projected.id AS projected_slice_id,
  extract_arg(projected.arg_set_id, 'gpu_render_stage_canonical_slice_id') AS canonical_slice_id,
  coalesce(
    extract_arg(projected_track.dimension_arg_set_id, 'upid'),
    extract_arg(projected_track.source_arg_set_id, 'gpu_process_upid')
  ) AS upid
FROM slice AS projected
JOIN track AS projected_track
  ON projected_track.id = projected.track_id
-- Projected slices are deliberately absent from gpu_slice, so identify their
-- TrackEvent tracks directly.
WHERE
  projected_track.type GLOB 'gpu*_track_event'
  AND extract_arg(projected.arg_set_id, 'gpu_render_stage_canonical_slice_id') IS NOT NULL;

CREATE PERFETTO INDEX _gpu_render_stage_projections_canonical ON _gpu_render_stage_projections(
  canonical_slice_id,
  upid
);
