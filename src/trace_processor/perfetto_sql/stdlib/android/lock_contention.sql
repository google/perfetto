INCLUDE PERFETTO MODULE android.monitor_contention;

-- Extracts the owner tid from a lock contention slice name.
CREATE PERFETTO FUNCTION android_extract_lock_contention_owner_tid(
    -- Name of the slice.
    slice_name STRING
)
-- The owner tid.
RETURNS LONG AS
SELECT
  cast_int!(STR_SPLIT(STR_SPLIT($slice_name, "(owner tid: ", 1), ")", 0));

-- Contains parsed lock contention events, including the owner tid and blocked/blocking thread names.
CREATE PERFETTO TABLE android_lock_contention (
  -- Slice ID.
  id LONG,
  -- Timestamp.
  ts LONG,
  -- Duration.
  dur LONG,
  -- Slice name.
  name STRING,
  -- Owner TID.
  owner_tid LONG,
  -- Blocked thread name.
  blocked_thread_name STRING,
  -- Blocking thread name.
  blocking_thread_name STRING
) AS
SELECT
  s.id,
  s.ts,
  s.dur,
  s.name,
  android_extract_lock_contention_owner_tid(s.name) AS owner_tid,
  bt.name AS blocked_thread_name,
  obt.name AS blocking_thread_name
FROM slice AS s
JOIN thread_track AS tt
  ON s.track_id = tt.id
JOIN thread AS bt
  USING (utid)
LEFT JOIN thread AS obt
  ON obt.tid = android_extract_lock_contention_owner_tid(s.name)
  AND obt.upid = bt.upid
WHERE
  s.name GLOB 'Lock contention*' AND s.name GLOB '*(owner tid: *)*';

-- Contains the union of all lock contention events from both ART and Monitor contention sources.
CREATE PERFETTO TABLE android_all_lock_contentions (
  -- Slice ID.
  id LONG,
  -- Timestamp.
  ts LONG,
  -- Duration.
  dur LONG,
  -- Slice name.
  name STRING,
  -- Owner TID.
  owner_tid LONG,
  -- Blocked thread name.
  blocked_thread_name STRING,
  -- Blocking thread name.
  blocking_thread_name STRING
) AS
SELECT
  id,
  ts,
  dur,
  '[Lock Owner] Blocking: ' || name AS name,
  owner_tid,
  blocked_thread_name,
  blocking_thread_name
FROM android_lock_contention
UNION ALL
SELECT
  id,
  ts,
  coalesce(dur, -1) AS dur,
  '[Lock Owner] Blocking: Lock contention on a monitor lock (owner tid: ' || coalesce(blocking_tid, '-') || ')' AS name,
  blocking_tid AS owner_tid,
  blocked_thread_name,
  blocking_thread_name
FROM android_monitor_contention_chain;

-- Index on owner_tid to speed up layout window functions.
CREATE PERFETTO INDEX android_all_lock_contentions_owner_tid ON android_all_lock_contentions(owner_tid);
