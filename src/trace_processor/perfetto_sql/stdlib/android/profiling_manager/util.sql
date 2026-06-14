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

-- Returns an artificially generated slice for each pair of (matched(startSlicePattern), matched(endSlicePattern))
-- Caveat: If multiple slices match endSlicePattern it will only return one per start slice and it will be closest slice in the timeline.
CREATE PERFETTO FUNCTION _android_generate_start_to_end_slices(
  -- GLOB pattern to find the start slices
  start_pattern STRING,
  -- GLOB pattern to find the matching end slice for each slice matched by startSlicePattern
  end_pattern STRING,
  -- Whether the generated slice should include the duration of the end slice or not
  inclusive BOOL
)
RETURNS TABLE(
  -- slice name
  name STRING,
  -- slice timestamp
  ts LONG,
  -- slice duration in nanoseconds
  dur LONG
)
AS
SUBPIPELINE end_slices AS (
  FROM slice
  |> WHERE name GLOB $end_pattern
)
FROM slice AS s
|> WHERE s.name GLOB $start_pattern
-- Attach the nearest end slice that begins after each start slice. Non-LEFT FIND
-- drops start slices with no following end slice (the old `dur IS NOT NULL` filter).
|> INTERVAL FIND end_slices AS e STARTING AFTER BEGIN
|> SELECT
     s.name,
     s.ts,
     e.ts + iif($inclusive, e.dur, 0) - s.ts AS dur;
