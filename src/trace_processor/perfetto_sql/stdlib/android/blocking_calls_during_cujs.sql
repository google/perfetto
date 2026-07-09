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
INCLUDE PERFETTO MODULE android.slices;

-- OPTIMIZED: Include CUJs module to filter by relevant processes
INCLUDE PERFETTO MODULE android.cujs.sysui_cujs;

INCLUDE PERFETTO MODULE android.critical_blocking_calls;

-- Extract critical blocking calls from processes that have CUJs.
-- Materialized as a table, heavily optimized using CROSS JOIN to force index
-- and avoid scanning the global slice table.
CREATE PERFETTO TABLE _android_blocking_calls_during_cujs AS
WITH relevant_upids AS (
  SELECT DISTINCT upid FROM android_jank_latency_cujs
)
SELECT
  android_standardize_slice_name(slice.name) AS name,
  slice.ts,
  slice.dur,
  slice.id,
  process.name AS process_name,
  thread.utid,
  process.upid,
  slice.ts + slice.dur AS ts_end
FROM relevant_upids ru
CROSS JOIN process ON process.upid = ru.upid
CROSS JOIN thread ON thread.upid = process.upid
CROSS JOIN thread_track ON thread_track.utid = thread.utid
CROSS JOIN slice ON slice.track_id = thread_track.id
WHERE
  _is_relevant_blocking_call(slice.name, slice.depth)
UNION ALL
-- Add a summation of all drawLayer slices without the individual layer name
SELECT
  'drawLayer' AS name,
  slice.ts,
  slice.dur,
  slice.id,
  process.name AS process_name,
  thread.utid,
  process.upid,
  slice.ts + slice.dur AS ts_end
FROM relevant_upids ru
CROSS JOIN process ON process.upid = ru.upid
CROSS JOIN thread ON thread.upid = process.upid
CROSS JOIN thread_track ON thread_track.utid = thread.utid
CROSS JOIN slice ON slice.track_id = thread_track.id
WHERE
  slice.name GLOB 'drawLayer *'
UNION ALL
-- As binder names are not included in slice table, extract these directly from the
-- android_binder_txns table via the base critical blocking calls view.
SELECT tx.*
FROM _android_critical_binder_calls tx
JOIN relevant_upids ru ON tx.upid = ru.upid;

CREATE PERFETTO INDEX _android_blocking_calls_during_cujs_idx ON _android_blocking_calls_during_cujs(utid, ts);
