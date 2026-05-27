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

INCLUDE PERFETTO MODULE android.binder;

INCLUDE PERFETTO MODULE android.critical_blocking_calls;

-- OPTIMIZED: Include CUJs module to filter by relevant processes
INCLUDE PERFETTO MODULE android.cujs.sysui_cujs;

-- Extract critical blocking calls from processes that have CUJs.
-- Materialized as a table, heavily optimized using CROSS JOIN to force index
-- and avoid scanning the global row slice table.
CREATE PERFETTO TABLE _android_blocking_calls_during_cujs AS
WITH relevant_upids AS (SELECT DISTINCT upid FROM android_jank_latency_cujs)
SELECT
  android_standardize_slice_name(slice.name) AS name,
  slice.ts,
  slice.dur,
  slice.id,
  process.name AS process_name,
  thread.utid,
  process.upid,
  slice.ts + slice.dur AS ts_end
FROM relevant_upids AS ru
CROSS JOIN process
  ON process.upid = ru.upid
CROSS JOIN thread
  ON thread.upid = process.upid
CROSS JOIN thread_track
  ON thread_track.utid = thread.utid
CROSS JOIN slice
  ON slice.track_id = thread_track.id
WHERE
  _is_relevant_blocking_call(slice.name, slice.depth)
UNION ALL
-- As binder names are not included in slice table, extract these directly from the
-- android_binder_txns table.
SELECT
  tx.aidl_name AS name,
  tx.client_ts AS ts,
  tx.client_dur AS dur,
  tx.binder_txn_id AS id,
  tx.client_process AS process_name,
  tx.client_utid AS utid,
  tx.client_upid AS upid,
  tx.client_ts + tx.client_dur AS ts_end
FROM relevant_upids AS ru
CROSS JOIN android_binder_txns AS tx
  ON tx.client_upid = ru.upid
WHERE
  NOT (aidl_name IS NULL)
  AND is_sync = 1;
