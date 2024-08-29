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
--
INCLUDE PERFETTO MODULE android.slices;
INCLUDE PERFETTO MODULE android.binder;
INCLUDE PERFETTO MODULE slices.with_context;

CREATE PERFETTO FUNCTION _is_relevant_blocking_call(name STRING, depth INT)
RETURNS BOOL AS SELECT
  $name = 'measure'
  OR $name = 'layout'
  OR $name = 'configChanged'
  OR $name = 'animation'
  OR $name = 'input'
  OR $name = 'traversal'
  OR $name = 'Contending for pthread mutex'
  OR $name = 'postAndWait'
  OR $name GLOB 'monitor contention with*'
  OR $name GLOB 'SuspendThreadByThreadId*'
  OR $name GLOB 'LoadApkAssetsFd*'
  OR $name GLOB '*binder transaction*'
  OR $name GLOB 'inflate*'
  OR $name GLOB 'Lock contention on*'
  OR $name GLOB 'android.os.Handler: kotlinx.coroutines*'
  OR $name GLOB 'relayoutWindow*'
  OR $name GLOB 'ImageDecoder#decode*'
  OR $name GLOB 'NotificationStackScrollLayout#onMeasure'
  OR $name GLOB 'ExpNotRow#*'
  OR $name GLOB 'GC: Wait For*'
  OR (
    -- Some top level handler slices
    $depth = 0
    AND $name NOT GLOB '*Choreographer*'
    AND $name NOT GLOB '*Input*'
    AND $name NOT GLOB '*input*'
    AND $name NOT GLOB 'android.os.Handler: #*'
    AND (
      -- Handler pattern heuristics
      $name GLOB '*Handler: *$*'
      OR $name GLOB '*.*.*: *$*'
      OR $name GLOB '*.*$*: #*'
    )
  );


--Extract critical blocking calls from all processes.
CREATE PERFETTO TABLE _android_critical_blocking_calls AS
SELECT
  android_standardize_slice_name(s.name) AS name,
  s.ts,
  s.dur,
  s.id,
  s.process_name,
  thread.utid,
  s.upid
FROM thread_slice s JOIN
thread USING (utid)
WHERE
  _is_relevant_blocking_call(s.name, s.depth)
UNION ALL
-- As binder names are not included in slice table, extract these directly from the
-- android_binder_txns table.
SELECT
  tx.aidl_name AS name,
  tx.client_ts AS ts,
  tx.client_dur AS dur,
  tx.binder_txn_id AS id,
  tx.client_process as process_name,
  tx.client_utid as utid,
  tx.client_upid as upid
FROM android_binder_txns AS tx
WHERE aidl_name IS NOT NULL AND is_sync = 1;
