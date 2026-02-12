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

-- Returns all slices that match the provided glob pattern
CREATE PERFETTO FUNCTION android_find_slices(
    -- GLOB pattern to find slices
    pattern STRING
)
RETURNS TABLE (
  -- slice name
  name STRING,
  -- slice timestamp
  ts LONG,
  -- slice duration in nanoseconds
  dur LONG
) AS
SELECT
  name,
  ts,
  dur
FROM slice
WHERE
  name GLOB $pattern;

-- Returns an artificially generated slice for each pair of (matched(startSlicePattern), matched(endSlicePattern))
-- Caveat: If multiple slices match endSlicePattern it will only return one per start slice and it will be closest slice in the timeline.
-- Note: Uses GLOB patterns.
CREATE PERFETTO FUNCTION android_generate_start_to_end_slices(
    -- GLOB pattern to find the start slices
    startslicepattern STRING,
    -- GLOB pattern to find the matching end slice for each slice matched by startSlicePattern
    endslicepattern STRING,
    -- Whether the generated slice should include the duration of the end slice or not
    inclusive BOOL
)
RETURNS TABLE (
  -- slice name
  name STRING,
  -- slice timestamp
  ts LONG,
  -- slice duration in nanoseconds
  dur LONG
) AS
SELECT
  name,
  ts,
  min(starttoenddur) AS dur
FROM (
  SELECT
    s.name AS name,
    s.ts AS ts,
    e.ts + iif($inclusive, e.dur, 0) - s.ts AS starttoenddur
  FROM android_find_slices($startslicepattern) AS s
  CROSS JOIN android_find_slices($endslicepattern) AS e
  WHERE
    starttoenddur > 0
)
GROUP BY
  name,
  ts;
