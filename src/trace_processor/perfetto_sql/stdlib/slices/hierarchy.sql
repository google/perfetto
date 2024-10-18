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

-- Delete rows from |slice_table| where the |column_name| value is NULL.
--
-- The |parent_id| of the remaining rows are adjusted to point to the closest
-- ancestor remaining. This keeps the trees as connected as possible,
-- allowing further graph analysis.
CREATE PERFETTO MACRO _slice_remove_nulls_and_reparent(
  -- Table or subquery containing a subset of the slice table. Required columns are
  -- (id INT64, parent_id INT64, depth UINT32, <column_name>).
  slice_table TableOrSubQuery,
  -- Column name for which a NULL value indicates the row will be deleted.
  column_name ColumnName)
  -- The returned table has the schema (id INT64, parent_id INT64, depth UINT32, <column_name>).
RETURNS TableOrSubQuery
AS (
  WITH _slice AS (
    SELECT * FROM $slice_table WHERE $column_name IS NOT NULL
  )
  SELECT
    id,
    parent_id,
    depth,
    $column_name
  FROM _slice
  WHERE depth = 0
  UNION ALL
  SELECT
    child.id,
    anc.id AS parent_id,
    MAX(IIF(parent.$column_name IS NULL, 0, anc.depth)) AS depth,
    child.$column_name
  FROM _slice child
  JOIN ancestor_slice(child.id) anc
  LEFT JOIN _slice parent
    ON parent.id = anc.id
  GROUP BY child.id
);
