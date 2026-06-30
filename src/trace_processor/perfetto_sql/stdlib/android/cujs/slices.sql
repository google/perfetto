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
--

INCLUDE PERFETTO MODULE android.cujs.boundaries;

INCLUDE PERFETTO MODULE android.cujs.threads;

-- Slices that overlap with the CUJ boundary in the app process.
CREATE PERFETTO VIEW _android_jank_cuj_slice AS
SELECT
  cuj_id,
  process.upid,
  process.name AS process_name,
  thread.utid,
  thread.name AS thread_name,
  slice.id,
  slice.ts,
  slice.dur,
  slice.track_id,
  slice.category,
  slice.name,
  slice.depth,
  slice.parent_id,
  slice.arg_set_id,
  slice.thread_ts,
  slice.thread_dur,
  slice.thread_instruction_count,
  slice.thread_instruction_delta,
  slice.cat,
  slice.slice_id,
  slice.ts + slice.dur AS ts_end
FROM _android_jank_cuj_boundary AS boundary
JOIN process USING (upid)
JOIN thread USING (upid)
JOIN thread_track USING (utid)
JOIN slice
  ON slice.track_id = thread_track.id
  -- Take slices which overlap even they started before the boundaries
  -- This is to be able to query slices that delayed start of a frame
  AND slice.ts + slice.dur >= boundary.ts
  AND slice.ts <= boundary.ts_end
WHERE
  slice.dur > 0;

-- Slices that overlap with the CUJ boundary on the main thread of the app process.
CREATE PERFETTO TABLE _android_jank_cuj_main_thread_slice(
  -- CUJ id.
  cuj_id LONG,
  -- Process id.
  upid JOINID(process.id),
  -- Thread id.
  utid JOINID(thread.id),
  -- Slice id.
  id ID(slice.id),
  -- Timestamp of the slice.
  ts TIMESTAMP,
  -- Duration of the slice.
  dur DURATION,
  -- Track id.
  track_id JOINID(track.id),
  -- Category of the slice.
  category STRING,
  -- Name of the slice.
  name STRING,
  -- Depth of the slice.
  depth LONG,
  -- Parent slice id.
  parent_id JOINID(slice.id),
  -- Argument set id.
  arg_set_id LONG,
  -- Thread timestamp.
  thread_ts TIMESTAMP,
  -- Thread duration.
  thread_dur DURATION,
  -- Thread instruction count.
  thread_instruction_count LONG,
  -- Thread instruction delta.
  thread_instruction_delta LONG,
  -- Legacy category column.
  cat STRING,
  -- Legacy slice id.
  slice_id LONG,
  -- End timestamp of the slice.
  ts_end TIMESTAMP
)
AS
SELECT
  cuj_id,
  upid,
  utid,
  slice.id,
  slice.ts,
  slice.dur,
  slice.track_id,
  slice.category,
  slice.name,
  slice.depth,
  slice.parent_id,
  slice.arg_set_id,
  slice.thread_ts,
  slice.thread_dur,
  slice.thread_instruction_count,
  slice.thread_instruction_delta,
  slice.cat,
  slice.slice_id,
  slice.ts + slice.dur AS ts_end
FROM _android_jank_cuj_main_thread_cuj_boundary AS boundary
JOIN thread_track USING (utid)
JOIN thread USING (utid)
JOIN slice
  ON slice.track_id = thread_track.id
  -- Take slices which overlap even they started before the boundaries
  -- This is to be able to query slices that delayed start of a frame
  AND slice.ts + slice.dur >= boundary.ts
  AND slice.ts <= boundary.ts_end
WHERE
  slice.dur > 0;

-- Slices that overlap with the CUJ boundary on the render thread of the app process.
CREATE PERFETTO TABLE _android_jank_cuj_render_thread_slice(
  -- CUJ id.
  cuj_id LONG,
  -- Process id.
  upid JOINID(process.id),
  -- Thread id.
  utid JOINID(thread.id),
  -- Slice id.
  id ID(slice.id),
  -- Timestamp of the slice.
  ts TIMESTAMP,
  -- Duration of the slice.
  dur DURATION,
  -- Track id.
  track_id JOINID(track.id),
  -- Category of the slice.
  category STRING,
  -- Name of the slice.
  name STRING,
  -- Depth of the slice.
  depth LONG,
  -- Parent slice id.
  parent_id JOINID(slice.id),
  -- Argument set id.
  arg_set_id LONG,
  -- Thread timestamp.
  thread_ts TIMESTAMP,
  -- Thread duration.
  thread_dur DURATION,
  -- Thread instruction count.
  thread_instruction_count LONG,
  -- Thread instruction delta.
  thread_instruction_delta LONG,
  -- Legacy category column.
  cat STRING,
  -- Legacy slice id.
  slice_id LONG,
  -- End timestamp of the slice.
  ts_end TIMESTAMP
)
AS
SELECT
  cuj_id,
  upid,
  utid,
  slice.id,
  slice.ts,
  slice.dur,
  slice.track_id,
  slice.category,
  slice.name,
  slice.depth,
  slice.parent_id,
  slice.arg_set_id,
  slice.thread_ts,
  slice.thread_dur,
  slice.thread_instruction_count,
  slice.thread_instruction_delta,
  slice.cat,
  slice.slice_id,
  slice.ts + slice.dur AS ts_end
FROM _android_jank_cuj_render_thread_cuj_boundary AS boundary
JOIN thread_track USING (utid)
JOIN thread USING (utid)
JOIN slice
  ON slice.track_id = thread_track.id
  -- Take slices which overlap even they started before the boundaries
  -- This is to be able to query slices that delayed start of a frame
  AND slice.ts + slice.dur >= boundary.ts
  AND slice.ts <= boundary.ts_end
WHERE
  slice.dur > 0;

-- Slices that overlap with the CUJ boundary in the SurfaceFlinger process.
CREATE PERFETTO VIEW _android_jank_cuj_sf_slice AS
SELECT
  cuj_id,
  upid,
  sf_process.name AS process_name,
  thread.utid,
  thread.name AS thread_name,
  slice.id,
  slice.ts,
  slice.dur,
  slice.track_id,
  slice.category,
  slice.name,
  slice.depth,
  slice.parent_id,
  slice.arg_set_id,
  slice.thread_ts,
  slice.thread_dur,
  slice.thread_instruction_count,
  slice.thread_instruction_delta,
  slice.cat,
  slice.slice_id,
  slice.ts + slice.dur AS ts_end
FROM _android_jank_cuj_sf_boundary AS sf_boundary
JOIN _android_jank_cuj_sf_process AS sf_process
JOIN thread USING (upid)
JOIN thread_track USING (utid)
JOIN slice
  ON slice.track_id = thread_track.id
  -- Take slices which overlap even they started before the boundaries
  -- This is to be able to query slices that delayed start of a frame
  AND slice.ts + slice.dur >= sf_boundary.ts
  AND slice.ts <= sf_boundary.ts_end
WHERE
  slice.dur > 0;

-- Slices that overlap with the CUJ boundary on the SurfaceFlinger main thread.
CREATE PERFETTO TABLE _android_jank_cuj_sf_main_thread_slice(
  -- CUJ id.
  cuj_id LONG,
  -- Process id.
  upid JOINID(process.id),
  -- Thread id.
  utid JOINID(thread.id),
  -- Slice id.
  id ID(slice.id),
  -- Timestamp of the slice.
  ts TIMESTAMP,
  -- Duration of the slice.
  dur DURATION,
  -- Track id.
  track_id JOINID(track.id),
  -- Category of the slice.
  category STRING,
  -- Name of the slice.
  name STRING,
  -- Depth of the slice.
  depth LONG,
  -- Parent slice id.
  parent_id JOINID(slice.id),
  -- Argument set id.
  arg_set_id LONG,
  -- Thread timestamp.
  thread_ts TIMESTAMP,
  -- Thread duration.
  thread_dur DURATION,
  -- Thread instruction count.
  thread_instruction_count LONG,
  -- Thread instruction delta.
  thread_instruction_delta LONG,
  -- Legacy category column.
  cat STRING,
  -- Legacy slice id.
  slice_id LONG,
  -- End timestamp of the slice.
  ts_end TIMESTAMP
)
AS
SELECT
  cuj_id,
  upid,
  utid,
  slice.id,
  slice.ts,
  slice.dur,
  slice.track_id,
  slice.category,
  slice.name,
  slice.depth,
  slice.parent_id,
  slice.arg_set_id,
  slice.thread_ts,
  slice.thread_dur,
  slice.thread_instruction_count,
  slice.thread_instruction_delta,
  slice.cat,
  slice.slice_id,
  slice.ts + slice.dur AS ts_end
FROM _android_jank_cuj_sf_main_thread_cuj_boundary AS boundary
JOIN thread_track USING (utid)
JOIN thread USING (utid)
JOIN slice
  ON slice.track_id = thread_track.id
  -- Take slices which overlap even they started before the boundaries
  -- This is to be able to query slices that delayed start of a frame
  AND slice.ts + slice.dur >= boundary.ts
  AND slice.ts <= boundary.ts_end
WHERE
  slice.dur > 0;

-- Slices that overlap with the RenderEngine frame boundaries.
CREATE PERFETTO TABLE _android_jank_cuj_sf_render_engine_slice(
  -- CUJ id.
  cuj_id LONG,
  -- Process id.
  upid JOINID(process.id),
  -- Thread id.
  utid JOINID(thread.id),
  -- Slice id.
  id ID(slice.id),
  -- Timestamp of the slice.
  ts TIMESTAMP,
  -- Duration of the slice.
  dur DURATION,
  -- Track id.
  track_id JOINID(track.id),
  -- Category of the slice.
  category STRING,
  -- Name of the slice.
  name STRING,
  -- Depth of the slice.
  depth LONG,
  -- Parent slice id.
  parent_id JOINID(slice.id),
  -- Argument set id.
  arg_set_id LONG,
  -- Thread timestamp.
  thread_ts TIMESTAMP,
  -- Thread duration.
  thread_dur DURATION,
  -- Thread instruction count.
  thread_instruction_count LONG,
  -- Thread instruction delta.
  thread_instruction_delta LONG,
  -- Legacy category column.
  cat STRING,
  -- Legacy slice id.
  slice_id LONG,
  -- End timestamp of the slice.
  ts_end TIMESTAMP
)
AS
SELECT
  cuj_id,
  upid,
  utid,
  slice.id,
  slice.ts,
  slice.dur,
  slice.track_id,
  slice.category,
  slice.name,
  slice.depth,
  slice.parent_id,
  slice.arg_set_id,
  slice.thread_ts,
  slice.thread_dur,
  slice.thread_instruction_count,
  slice.thread_instruction_delta,
  slice.cat,
  slice.slice_id,
  slice.ts + slice.dur AS ts_end
FROM _android_jank_cuj_sf_render_engine_frame_boundary AS boundary
JOIN thread_track USING (utid)
JOIN thread USING (utid)
JOIN slice
  ON slice.track_id = thread_track.id
  -- Take slices which overlap even they started before the boundaries
  -- This is to be able to query slices that delayed start of a frame
  AND slice.ts + slice.dur >= boundary.ts
  AND slice.ts <= boundary.ts_end
WHERE
  slice.dur > 0;
