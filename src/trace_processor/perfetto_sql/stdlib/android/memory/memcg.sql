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

INCLUDE PERFETTO MODULE counters.intervals;

-- The memory cgroup (memcg) counter tracks recorded by the `linux.cgroup` data
-- source: one track per cgroup and memcg value, for the cgroups that Android
-- puts processes in (`/sys/fs/cgroup/{apps,system}/uid_<uid>/pid_<pid>`), the
-- cgroups of the uids above them and the top-level `apps` and `system`
-- cgroups.
CREATE PERFETTO TABLE android_memcg_track(
  -- Id of the counter track. Joinable with `counter_track.id`.
  id JOINID(track.id),
  -- Level of the cgroup in the hierarchy: 'process' for the cgroup of a
  -- process (`pid_<pid>`), 'uid' for the cgroup of a uid (`uid_<uid>`) and
  -- 'group' for the top-level `apps` and `system` cgroups.
  level STRING,
  -- What the value is: 'size' for sizes in bytes (e.g. `current`,
  -- `stat.anon`), 'limit' for limits and protections in bytes (e.g. `high`;
  -- -1 for "max", i.e. no limit) and 'event' for cumulative event counts (e.g.
  -- `events.local.high`, `stat.pgsteal`).
  kind STRING,
  -- The top-level cgroup that the cgroup is in: 'apps' or 'system'.
  memcg_group STRING,
  -- The uid of the cgroup, or of the cgroup above it for a process. NULL for
  -- the top-level cgroups.
  uid LONG,
  -- The process of the cgroup. NULL unless `level` is 'process'.
  upid JOINID(process.id),
  -- The memcg value: the name of the file without the `memory.` prefix,
  -- followed by the key of the line for files with several values (e.g.
  -- `current`, `swap.max`, `stat.file`, `events.local.high`).
  key STRING,
  -- Name of the track.
  name STRING,
  -- Unit of the values: 'bytes' for sizes and limits, NULL for event counts.
  unit STRING,
  -- Name of a package of the uid (compared modulo 100000, so that the uids
  -- of secondary users match too). NULL if there is none, and for uids below
  -- 10000.
  package_name STRING,
  -- An integer that identifies the cgroup, to partition by: one per distinct
  -- `level`, `memcg_group`, `uid` and `upid`. Not stable across traces or
  -- trace processor versions.
  cgroup_key LONG
)
AS
WITH
  tracks AS (
    SELECT
      id,
      type,
      name,
      unit,
      extract_arg(dimension_arg_set_id, 'memcg_group') AS memcg_group,
      extract_arg(dimension_arg_set_id, 'uid') AS uid,
      extract_arg(dimension_arg_set_id, 'upid') AS upid,
      extract_arg(dimension_arg_set_id, 'memcg_key') AS key
    FROM counter_track
    WHERE
      type IN (
        'memcg_process',
        'memcg_process_limit',
        'memcg_process_event',
        'memcg_uid',
        'memcg_uid_limit',
        'memcg_uid_event',
        'memcg_group',
        'memcg_group_limit',
        'memcg_group_event'
      )
  ),
  typed AS (
    SELECT
      *,
      CASE
        WHEN type GLOB 'memcg_process*' THEN 'process'
        WHEN type GLOB 'memcg_uid*' THEN 'uid'
        ELSE 'group'
      END AS level,
      CASE
        WHEN type GLOB '*_limit' THEN 'limit'
        WHEN type GLOB '*_event' THEN 'event'
        ELSE 'size'
      END AS kind
    FROM tracks
  )
SELECT
  t.id,
  t.level,
  t.kind,
  t.memcg_group,
  t.uid,
  t.upid,
  t.key,
  t.name,
  t.unit,
  -- A uid can have several packages (shared uids): pick one.
  (
    SELECT min(p.package_name)
    FROM package_list AS p
    WHERE
      p.uid = t.uid % 100000
      AND p.uid >= 10000
  ) AS package_name,
  -- Not the upid alone for processes: a pid reused under another uid has the
  -- same upid when the trace doesn't say when processes end, but is another
  -- cgroup.
  dense_rank() OVER (ORDER BY t.level, t.memcg_group, t.uid, t.upid) AS cgroup_key
FROM typed AS t;

-- The intervals of the memcg counters in |counters| (with the columns `id`,
-- `ts`, `track_id` and `value` of `counter`), with the columns of
-- `android_memcg_intervals`.
CREATE PERFETTO MACRO _android_memcg_intervals(counters TableOrSubquery)
RETURNS TableOrSubquery
AS (
  SELECT
    id,
    ts,
    end_ts - ts AS dur,
    track_id,
    cgroup_key,
    level,
    kind,
    memcg_group,
    uid,
    upid,
    key,
    -- Only after counter_leading_intervals!, which reads NULL as 0: a limit
    -- going from a value to "max" (-1) must end the interval of the value.
    iif(kind = 'limit' AND value < 0, NULL, value) AS value,
    iif(kind = 'limit', NULL, delta_value) AS delta_value
  FROM (
    SELECT
      i.id,
      max(i.ts, coalesce(p.start_ts, i.ts)) AS ts,
      min(i.ts + i.dur, coalesce(p.end_ts, i.ts + i.dur)) AS end_ts,
      i.track_id,
      t.cgroup_key,
      t.level,
      t.kind,
      t.memcg_group,
      t.uid,
      t.upid,
      t.key,
      i.value,
      i.delta_value
    FROM counter_leading_intervals!($counters) AS i
    JOIN android_memcg_track AS t
      ON i.track_id = t.id
    LEFT JOIN process AS p
      ON t.upid = p.upid
  )
  WHERE
    end_ts > ts
);

-- The values of the memcg counter tracks of `android_memcg_track` as
-- intervals: each value holds until the next value of the track. When the data
-- source records only the values that changed (`cache_ttl_ms`), the intervals
-- are as long as the values didn't change.
--
-- The intervals of a process's cgroup are clipped to the lifetime of the
-- process, when it's known. The others last until the end of the trace.
--
-- This is a view, computed whenever it's queried: materialise it (e.g. with
-- `CREATE PERFETTO TABLE`) when querying it more than once, ideally restricted
-- to the tracks needed.
CREATE PERFETTO VIEW android_memcg_intervals(
  -- Id of the counter value at the start of the interval. Joinable with
  -- `counter.id`.
  id JOINID(counter.id),
  -- Start of the interval.
  ts TIMESTAMP,
  -- Duration of the interval.
  dur DURATION,
  -- Id of the counter track. Joinable with `android_memcg_track.id`.
  track_id JOINID(track.id),
  -- An integer that identifies the cgroup, see `android_memcg_track`.
  cgroup_key LONG,
  -- Level of the cgroup: 'process', 'uid' or 'group'.
  level STRING,
  -- What the value is: 'size', 'limit' or 'event'.
  kind STRING,
  -- The top-level cgroup: 'apps' or 'system'.
  memcg_group STRING,
  -- The uid of the cgroup. NULL for the top-level cgroups.
  uid LONG,
  -- The process of the cgroup. NULL unless `level` is 'process'.
  upid JOINID(process.id),
  -- The memcg value, e.g. `current` or `stat.anon`.
  key STRING,
  -- The value during the interval. NULL for a limit set to "max" (no limit).
  value DOUBLE,
  -- The difference to the previous value of the track: the number of events
  -- since the previous value for event counts. NULL for the first value of a
  -- track, and for limits.
  delta_value DOUBLE
)
AS
SELECT *
FROM _android_memcg_intervals!(
  (
    SELECT c.id, c.ts, c.track_id, c.value
    FROM counter AS c
    WHERE
      c.track_id IN (SELECT id FROM android_memcg_track)
  )
);
