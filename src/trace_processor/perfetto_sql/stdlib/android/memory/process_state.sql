--
-- Copyright 2026 The Android Open Source Project
--
-- Licensed under the Apache License, Version 2.0 (the 'License');
-- you may not use this file except in compliance with the License.
-- You may obtain a copy of the License at
--
--     https://www.apache.org/licenses/LICENSE-2.0
--
-- Unless required by applicable law or agreed to in writing, software
-- distributed under the License is distributed on an 'AS IS' BASIS,
-- WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
-- See the License for the specific language governing permissions and
-- limitations under the License.

-- Helpers for the android.process_state data source. The producer
-- can run in delta-snapshot mode (every Nth snapshot is a full
-- "anchor"; intermediate snapshots only carry rows whose adj-relevant
-- state changed). A naive `WHERE snapshot_id = X` against a delta
-- snapshot returns only the changed rows — these macros do the
-- per-pid reconstruction so callers see full state at any snapshot.

-- Latest snapshot at or before a given boot-time timestamp.
CREATE PERFETTO FUNCTION android_process_state_snapshot_at(
  -- Boot-time timestamp in nanoseconds.
  ts LONG
)
-- Snapshot id closest to (but not after) ts; NULL if none exists.
RETURNS LONG
AS
SELECT id
FROM android_process_state_snapshot
WHERE
  ts <= $ts
ORDER BY
  ts DESC
LIMIT 1;

-- Reconstruct full per-pid process state at a given snapshot id, by
-- taking the most recent row per pid at-or-before snap_id. Works
-- whether the trace was full-anchors-only or delta-with-anchors.
CREATE PERFETTO FUNCTION android_process_state_full_state_at(
  -- Snapshot id to reconstruct full state at.
  snap_id LONG
)
RETURNS TABLE(
  -- Process id (kernel TID of the main thread).
  pid LONG,
  -- Process app uid.
  uid LONG,
  -- Android user id (typically 0; >0 for secondary users).
  user_id LONG,
  -- Process name from /proc/<pid>/cmdline (interned).
  process_name STRING,
  -- Owning package name (interned).
  package_name STRING,
  -- Current oom_adj value (lower = higher priority).
  cur_adj LONG,
  -- Set oom_adj — the value the OomAdjuster has decided to apply
  -- but may not have written to /proc/<pid>/oom_score_adj yet.
  set_adj LONG,
  -- Maximum oom_adj this process is allowed to reach.
  max_adj LONG,
  -- ActivityManager.PROCESS_STATE_*.
  cur_proc_state LONG,
  -- Capability bitmask (PROCESS_CAPABILITY_*).
  cur_capability LONG,
  -- ProcessList.SCHED_GROUP_*.
  cur_sched_group LONG,
  -- 1 if process is at PERSISTENT_PROC_ADJ.
  persistent LONG,
  -- 1 if process has top-ui foreground UI window.
  has_top_ui LONG,
  -- 1 if process owns an overlay window.
  has_overlay_ui LONG,
  -- 1 if process has visible activities.
  has_visible_activities LONG,
  -- 1 if process has foreground activities.
  has_foreground_activities LONG,
  -- 1 if process has any started services.
  has_started_services LONG,
  -- 1 if process is a sandbox isolated child.
  isolated LONG,
  -- 1 if process has active instrumentation.
  has_active_instrumentation LONG,
  -- LRU position in mLruProcesses (0 = most recently used).
  lru_index LONG,
  -- The snapshot this row was originally observed in (latest at or
  -- before snap_id).
  source_snapshot_id LONG
)
AS
WITH
  latest AS (
    SELECT
      p.*,
      ROW_NUMBER() OVER (PARTITION BY pid ORDER BY snapshot_id DESC) AS rn
    FROM android_process_state_process AS p
    WHERE
      p.snapshot_id <= $snap_id
  )
SELECT
  pid,
  uid,
  user_id,
  process_name,
  package_name,
  cur_adj,
  set_adj,
  max_adj,
  cur_proc_state,
  cur_capability,
  cur_sched_group,
  persistent,
  has_top_ui,
  has_overlay_ui,
  has_visible_activities,
  has_foreground_activities,
  has_started_services,
  isolated,
  has_active_instrumentation,
  lru_index,
  snapshot_id AS source_snapshot_id
FROM latest
WHERE
  rn = 1;

-- Reconstruct full inbound bindings at a given snapshot id (joined to
-- the owning service so each row is a (client_pid → service_pid,
-- service_short_name, bind_flags) tuple).
CREATE PERFETTO FUNCTION android_process_state_bindings_at(
  -- Snapshot id to reconstruct bindings at.
  snap_id LONG
)
RETURNS TABLE(
  -- Client process id (the binder of the binding).
  client_pid LONG,
  -- Service-owning process id.
  service_pid LONG,
  -- ComponentName.shortString of the service.
  service_short_name STRING,
  -- Raw bind() flags bitmask (Context.BIND_*).
  flags LONG,
  -- 1 if BIND_AUTO_CREATE.
  flag_auto_create LONG,
  -- 1 if BIND_FOREGROUND_SERVICE.
  flag_foreground_service LONG,
  -- 1 if BIND_NOT_FOREGROUND.
  flag_not_foreground LONG,
  -- 1 if BIND_ABOVE_CLIENT.
  flag_above_client LONG,
  -- 1 if BIND_ALLOW_OOM_MANAGEMENT.
  flag_allow_oom_management LONG,
  -- 1 if BIND_WAIVE_PRIORITY (binding does NOT propagate priority).
  flag_waive_priority LONG,
  -- 1 if BIND_IMPORTANT.
  flag_important LONG,
  -- 1 if BIND_ADJUST_WITH_ACTIVITY.
  flag_adjust_with_activity LONG,
  -- 1 if BIND_INCLUDE_CAPABILITIES.
  flag_include_capabilities LONG
)
AS
WITH
  latest_b AS (
    SELECT
      b.*,
      ROW_NUMBER() OVER (PARTITION BY binding_id ORDER BY snapshot_id DESC) AS rn
    FROM android_process_state_binding AS b
    WHERE
      b.snapshot_id <= $snap_id
  ),
  latest_s AS (
    SELECT
      s.*,
      ROW_NUMBER() OVER (PARTITION BY service_id ORDER BY snapshot_id DESC) AS rn
    FROM android_process_state_service AS s
    WHERE
      s.snapshot_id <= $snap_id
  )
SELECT
  b.client_pid,
  s.owning_pid AS service_pid,
  s.short_name AS service_short_name,
  b.flags,
  b.flag_auto_create,
  b.flag_foreground_service,
  b.flag_not_foreground,
  b.flag_above_client,
  b.flag_allow_oom_management,
  b.flag_waive_priority,
  b.flag_important,
  b.flag_adjust_with_activity,
  b.flag_include_capabilities
FROM latest_b AS b
LEFT JOIN latest_s AS s
  ON s.service_id = b.service_id
  AND s.rn = 1
WHERE
  b.rn = 1
  AND b.client_pid IS NOT NULL
  AND s.owning_pid IS NOT NULL
  AND b.client_pid != s.owning_pid;
