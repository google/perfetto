import re

stdlib_path = "src/trace_processor/perfetto_sql/stdlib/android/cujs/slices.sql"
metrics_path = "src/trace_processor/metrics/sql/android/jank/slices.sql"
build_path = "src/trace_processor/perfetto_sql/stdlib/android/cujs/BUILD.gn"

app_slices_str = """--
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

INCLUDE PERFETTO MODULE android.cujs.boundaries;
INCLUDE PERFETTO MODULE android.cujs.threads;

-- Slices overlapping with the CUJ boundaries in the app processes.
CREATE PERFETTO TABLE _android_jank_cuj_slice(
  -- Unique incremental ID for each CUJ.
  cuj_id LONG,
  -- Process ID of the process.
  upid JOINID(process.id),
  -- Thread ID of the thread.
  utid JOINID(thread.id),
  -- Slice id.
  id ID(slice.id),
  -- Timestamp of the slice.
  ts TIMESTAMP,
  -- Duration of the slice.
  dur LONG,
  -- Track id of the slice.
  track_id JOINID(track.id),
  -- Category of the slice.
  category STRING,
  -- Name of the slice.
  name STRING,
  -- Depth of the slice in the stack.
  depth LONG,
  -- Parent slice id.
  parent_id LONG,
  -- Arg set id.
  arg_set_id LONG,
  -- Thread timestamp.
  thread_ts TIMESTAMP,
  -- Thread duration.
  thread_dur LONG,
  -- Thread instruction count.
  thread_instruction_count LONG,
  -- Thread instruction delta.
  thread_instruction_delta LONG,
  -- Legacy category alias.
  cat STRING,
  -- Legacy slice id alias.
  slice_id LONG,
  -- End timestamp of the slice.
  ts_end TIMESTAMP
)
AS
SELECT
  cuj_id,
  upid,
  utid,
  slice.*,
  slice.ts + slice.dur AS ts_end
FROM _android_jank_cuj_boundary boundary
JOIN thread_track USING (utid)
JOIN thread USING (utid)
JOIN slice
  ON slice.track_id = thread_track.id
    -- Take slices which overlap even they started before the boundaries
    -- This is to be able to query slices that delayed start of a frame
    AND slice.ts + slice.dur >= boundary.ts
    AND slice.ts <= boundary.ts_end
WHERE slice.dur > 0;

-- Slices overlapping with the main thread CUJ boundaries in the app processes.
CREATE PERFETTO TABLE _android_jank_cuj_main_thread_slice(
  -- Unique incremental ID for each CUJ.
  cuj_id LONG,
  -- Process ID of the process.
  upid JOINID(process.id),
  -- Thread ID of the thread.
  utid JOINID(thread.id),
  -- Slice id.
  id ID(slice.id),
  -- Timestamp of the slice.
  ts TIMESTAMP,
  -- Duration of the slice.
  dur LONG,
  -- Track id of the slice.
  track_id JOINID(track.id),
  -- Category of the slice.
  category STRING,
  -- Name of the slice.
  name STRING,
  -- Depth of the slice in the stack.
  depth LONG,
  -- Parent slice id.
  parent_id LONG,
  -- Arg set id.
  arg_set_id LONG,
  -- Thread timestamp.
  thread_ts TIMESTAMP,
  -- Thread duration.
  thread_dur LONG,
  -- Thread instruction count.
  thread_instruction_count LONG,
  -- Thread instruction delta.
  thread_instruction_delta LONG,
  -- Legacy category alias.
  cat STRING,
  -- Legacy slice id alias.
  slice_id LONG,
  -- End timestamp of the slice.
  ts_end TIMESTAMP
)
AS
SELECT
  cuj_id,
  upid,
  utid,
  slice.*,
  slice.ts + slice.dur AS ts_end
FROM _android_jank_cuj_main_thread_cuj_boundary boundary
JOIN thread_track USING (utid)
JOIN thread USING (utid)
JOIN slice
  ON slice.track_id = thread_track.id
    -- Take slices which overlap even they started before the boundaries
    -- This is to be able to query slices that delayed start of a frame
    AND slice.ts + slice.dur >= boundary.ts
    AND slice.ts <= boundary.ts_end
WHERE slice.dur > 0;

-- Slices overlapping with the render thread CUJ boundaries in the app processes.
CREATE PERFETTO TABLE _android_jank_cuj_render_thread_slice(
  -- Unique incremental ID for each CUJ.
  cuj_id LONG,
  -- Process ID of the process.
  upid JOINID(process.id),
  -- Thread ID of the thread.
  utid JOINID(thread.id),
  -- Slice id.
  id ID(slice.id),
  -- Timestamp of the slice.
  ts TIMESTAMP,
  -- Duration of the slice.
  dur LONG,
  -- Track id of the slice.
  track_id JOINID(track.id),
  -- Category of the slice.
  category STRING,
  -- Name of the slice.
  name STRING,
  -- Depth of the slice in the stack.
  depth LONG,
  -- Parent slice id.
  parent_id LONG,
  -- Arg set id.
  arg_set_id LONG,
  -- Thread timestamp.
  thread_ts TIMESTAMP,
  -- Thread duration.
  thread_dur LONG,
  -- Thread instruction count.
  thread_instruction_count LONG,
  -- Thread instruction delta.
  thread_instruction_delta LONG,
  -- Legacy category alias.
  cat STRING,
  -- Legacy slice id alias.
  slice_id LONG,
  -- End timestamp of the slice.
  ts_end TIMESTAMP
)
AS
SELECT
  cuj_id,
  upid,
  utid,
  slice.*,
  slice.ts + slice.dur AS ts_end
FROM _android_jank_cuj_render_thread_cuj_boundary boundary
JOIN thread_track USING (utid)
JOIN thread USING (utid)
JOIN slice
  ON slice.track_id = thread_track.id
    -- Take slices which overlap even they started before the boundaries
    -- This is to be able to query slices that delayed start of a frame
    AND slice.ts + slice.dur >= boundary.ts
    AND slice.ts <= boundary.ts_end
WHERE slice.dur > 0;
"""

with open(stdlib_path, "w") as f:
    f.write(app_slices_str)

with open(metrics_path, "r") as f:
    m = f.read()

m = getattr(re.compile(r'DROP TABLE IF EXISTS android_jank_cuj_slice.*?FROM boundary_base;', re.DOTALL), 'sub')("DROP TABLE IF EXISTS android_jank_cuj_slice;\nCREATE PERFETTO TABLE android_jank_cuj_slice AS\nSELECT * FROM _android_jank_cuj_slice;", m)
m = getattr(re.compile(r'DROP TABLE IF EXISTS android_jank_cuj_main_thread_slice.*?WHERE slice\.dur > 0;', re.DOTALL), 'sub')("DROP TABLE IF EXISTS android_jank_cuj_main_thread_slice;\nCREATE PERFETTO TABLE android_jank_cuj_main_thread_slice AS\nSELECT * FROM _android_jank_cuj_main_thread_slice;", m)
m = getattr(re.compile(r'DROP TABLE IF EXISTS android_jank_cuj_render_thread_slice.*?WHERE slice\.dur > 0;', re.DOTALL), 'sub')("DROP TABLE IF EXISTS android_jank_cuj_render_thread_slice;\nCREATE PERFETTO TABLE android_jank_cuj_render_thread_slice AS\nSELECT * FROM _android_jank_cuj_render_thread_slice;", m)
m = m.replace("INCLUDE PERFETTO MODULE android.cujs.boundaries;", "INCLUDE PERFETTO MODULE android.cujs.boundaries;\nINCLUDE PERFETTO MODULE android.cujs.slices;")

with open(metrics_path, "w") as f:
    f.write(m)

with open(build_path, "r") as f:
    b = f.read()

if 'slices.sql' not in b:
    b = b.replace('"relevant_slices.sql",', '"relevant_slices.sql",\n    "slices.sql",')
with open(build_path, "w") as f:
    f.write(b)
