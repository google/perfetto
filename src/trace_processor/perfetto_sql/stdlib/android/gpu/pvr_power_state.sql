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

-- GPU power state which is analogous to CPU idle state
CREATE PERFETTO TABLE android_pvr_gpu_power_state (
  -- Timestamp
  ts TIMESTAMP,
  -- Duration
  dur DURATION,
  -- GPU power state
  power_state LONG
) AS
SELECT
  s.ts,
  -- Fix unfinished slices (dur=-1) by clamping to trace end
  CASE
    WHEN s.dur = -1
    THEN (
      SELECT
        end_ts
      FROM trace_bounds
    ) - s.ts
    ELSE s.dur
  END AS dur,
  -- Map slice names to integer states
  CASE s.name WHEN 'OFF' THEN 0 WHEN 'PG' THEN 1 WHEN 'ON' THEN 2 ELSE -1 END AS power_state
FROM slice AS s
JOIN track AS t
  ON s.track_id = t.id
WHERE
  t.name = 'powervr_gpu_power_state';
