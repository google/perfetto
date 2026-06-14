--
-- Copyright 2025 The Android Open Source Project
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

-- For every slice in the `slice` table, computes the "self-duration": the time
-- spent in the slice but *not* spent in any child slices.
CREATE PERFETTO PIPELINE slice_self_dur(
  -- The id of the slice.
  id ID(slice.id),
  -- The self duration for the slice: the time spent in the slice but not any
  -- child slices.
  self_dur DURATION
) MATERIALIZED AS
SUBPIPELINE children_dur AS (
  FROM slice
  |> WHERE parent_id IS NOT NULL
  |> AGGREGATE SUM(dur) AS child_dur_sum GROUP BY parent_id
  |> SELECT parent_id AS id, child_dur_sum
)
FROM slice
|> LEFT JOIN children_dur USING (id)
|> SELECT slice.id, slice.dur - coalesce(child_dur_sum, 0) AS self_dur;
