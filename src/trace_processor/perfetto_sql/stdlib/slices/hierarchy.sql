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

-- Similar to `ancestor_slice`, but returns the slice itself in addition to strict ancestors.
CREATE PERFETTO FUNCTION _slice_ancestor_and_self(
  -- Id of the slice.
  slice_id LONG
)
RETURNS TABLE(
  -- Alias of `slice.id`.
  id LONG,
  -- Alias of `slice.type`.
  type STRING,
  -- Alias of `slice.ts`.
  ts LONG,
  -- Alias of `slice.dur`.
  dur LONG,
  -- Alias of `slice.track_id`.
  track_id LONG,
  -- Alias of `slice.category`.
  category STRING,
  -- Alias of `slice.name`.
  name STRING,
  -- Alias of `slice.depth`.
  depth LONG,
  -- Alias of `slice.parent_id`.
  parent_id LONG,
  -- Alias of `slice.arg_set_id`.
  arg_set_id LONG,
  -- Alias of `slice.thread_ts`.
  thread_ts LONG,
  -- Alias of `slice.thread_dur`.
  thread_dur LONG
) AS
SELECT
  id, type, ts, dur, track_id, category, name, depth, parent_id, arg_set_id, thread_ts, thread_dur
FROM slice
WHERE id = $slice_id
UNION ALL
SELECT
  id, type, ts, dur, track_id, category, name, depth, parent_id, arg_set_id, thread_ts, thread_dur
FROM ancestor_slice($slice_id);

-- Similar to `descendant_slice`, but returns the slice itself in addition to strict descendants.
CREATE PERFETTO FUNCTION _slice_descendant_and_self(
  -- Id of the slice.
  slice_id LONG
)
RETURNS TABLE(
  -- Alias of `slice.id`.
  id LONG,
  -- Alias of `slice.type`.
  type STRING,
  -- Alias of `slice.ts`.
  ts LONG,
  -- Alias of `slice.dur`.
  dur LONG,
  -- Alias of `slice.track_id`.
  track_id LONG,
  -- Alias of `slice.category`.
  category STRING,
  -- Alias of `slice.name`.
  name STRING,
  -- Alias of `slice.depth`.
  depth LONG,
  -- Alias of `slice.parent_id`.
  parent_id LONG,
  -- Alias of `slice.arg_set_id`.
  arg_set_id LONG,
  -- Alias of `slice.thread_ts`.
  thread_ts LONG,
  -- Alias of `slice.thread_dur`.
  thread_dur LONG
) AS
SELECT
  id, type, ts, dur, track_id, category, name, depth, parent_id, arg_set_id, thread_ts, thread_dur
FROM slice
WHERE id = $slice_id
UNION ALL
SELECT
  id, type, ts, dur, track_id, category, name, depth, parent_id, arg_set_id, thread_ts, thread_dur
FROM descendant_slice($slice_id);