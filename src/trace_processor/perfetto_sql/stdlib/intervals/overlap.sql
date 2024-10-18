--
-- Copyright 2023 The Android Open Source Project
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

INCLUDE PERFETTO MODULE intervals.intersect;

-- Compute the distribution of the overlap of the given intervals over time.
--
-- Each interval is a (ts, dur) pair and the overlap represented as a (ts, value)
-- counter, with the value corresponding to the number of intervals that overlap
-- the given timestamp and interval until the next timestamp.
CREATE PERFETTO MACRO intervals_overlap_count(
    -- Table or subquery containing interval data.
    segments TableOrSubquery,
    -- Column containing interval starts (usually `ts`).
    ts_column ColumnName,
    -- Column containing interval durations (usually `dur`).
    dur_column ColumnName)
-- The returned table has the schema (ts INT64, value UINT32).
-- |ts| is the timestamp when the number of open segments changed. |value| is
-- the number of open segments.
RETURNS TableOrSubquery AS
(
-- Algorithm: for each segment, emit a +1 at the start and a -1 at the end.
-- Then, merge events with the same timestamp and compute a cumulative sum.
WITH
_starts AS (
  SELECT
    1 AS delta,
    $ts_column AS ts
  FROM $segments
),
_ends AS (
  SELECT
    -1 AS delta,
    $ts_column + $dur_column AS ts
  FROM $segments
  WHERE $dur_column != -1
),
_events AS (
  SELECT * FROM _starts
  UNION ALL
  SELECT * FROM _ends
),
-- Merge events with the same timestamp to avoid artifacts in the data.
_merged_events AS (
  SELECT ts, sum(delta) as delta
  FROM _events
  GROUP BY ts
)
SELECT
  ts,
  sum(delta) OVER (
    ORDER BY ts
    ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW
  ) as value
FROM _merged_events
ORDER BY ts
);

-- Returns whether |intervals| contains any overlapping intervals. Useful for
-- checking if provided table/subquery can be used for intervals_intersect
-- macro.
CREATE PERFETTO MACRO _intervals_overlap_in_table(
  -- Table/subquery of intervals with |ts| and |dur| columns.
  intervals TableOrSubquery)
-- Returns 1 if table contains overlapping intervals. Otherwise returns 0.
RETURNS Expr AS (
WITH ts_with_next AS (
  SELECT
    ts + dur AS ts_end,
    -- The last slice will have |next_ts == NULL|, but it's not an issue as if
    -- it's the last slice we know that it will not overlap with the next one.
    LEAD(ts) OVER (ORDER BY ts) AS next_ts
  FROM $intervals
  WHERE dur != -1
), filtered AS (
  SELECT * FROM ts_with_next
  WHERE ts_end > next_ts
  LIMIT 1
)
SELECT count() AS has_overlaps
FROM filtered
);

-- Merges a |roots_table| and |children_table| into one table. See _intervals_flatten
-- that accepts the output of this macro to flatten intervals.

-- See: _intervals_merge_root_and_children_by_intersection.
CREATE PERFETTO MACRO _intervals_merge_root_and_children(
  -- Table or subquery containing all the root intervals: (id, ts, dur).
  -- Note that parent_id is not necessary in this table as it will be NULL anyways.
  roots_table TableOrSubquery,
  -- Table or subquery containing all the child intervals:
  -- (root_id, id, parent_id, ts, dur)
  children_table TableOrSubquery)
-- The returned table has the schema (root_id UINT32, root_ts INT64, root_dur, INT64,
-- id UINT32, parent_id UINT32, ts INT64, dur INT64).
RETURNS TableOrSubquery
AS (
  WITH
    _roots AS (
      SELECT id AS root_id, ts AS root_ts, dur AS root_dur FROM ($roots_table) WHERE dur > 0
    ),
    _children AS (
      SELECT * FROM ($children_table) WHERE dur > 0
    ),
    _roots_without_children AS (
      SELECT root_id FROM _roots
      EXCEPT
      SELECT DISTINCT parent_id AS root_id FROM _children
    )
    SELECT
      _roots.root_id,
      _roots.root_ts,
      _roots.root_dur,
      _children.id,
      _children.parent_id,
      _children.ts,
      _children.dur
    FROM _children
    JOIN _roots USING(root_id)
    UNION ALL
    -- Handle singleton roots
    SELECT
      root_id,
      root_ts,
      root_dur,
      NULL AS id,
      NULL AS parent_id,
      NULL AS ts,
      NULL AS dur
    FROM _roots_without_children
    JOIN _roots USING(root_id)
);

-- Merges a |roots_table| and |children_table| into one table. See _intervals_flatten
-- that accepts the output of this macro to flatten intervals.

-- This is very similar to _intervals_merge_root_and_children but there is no explicit
-- root_id shared between the root and the children. Instead an _interval_intersect is
-- used to derive the root and child relationships.
CREATE PERFETTO MACRO _intervals_merge_root_and_children_by_intersection(
  -- Table or subquery containing all the root intervals: (id, ts, dur).
  -- Note that parent_id is not necessary in this table as it will be NULL anyways.
  roots_table TableOrSubquery,
  -- Table or subquery containing all the child intervals:
  -- (root_id, id, parent_id, ts, dur)
  children_table TableOrSubquery,
  -- intersection key used in deriving the root child relationships.
  key ColumnName)
RETURNS TableOrSubQuery
AS (
  WITH
    _roots AS (
      SELECT * FROM $roots_table WHERE dur > 0 ORDER BY ts
    ),
    _children AS (
      SELECT * FROM $children_table WHERE dur > 0 ORDER BY ts
    )
    SELECT
      ii.ts,
      ii.dur,
      _children.id,
      IIF(_children.parent_id IS NULL, id_1, _children.parent_id) AS parent_id,
      _roots.id AS root_id,
      _roots.ts AS root_ts,
      _roots.dur AS root_dur,
      ii.$key
    FROM _interval_intersect!((_children, _roots), ($key)) ii
    JOIN _children
      ON _children.id = id_0
    JOIN _roots
      ON _roots.id = id_1
);

-- Partition and flatten a hierarchy of intervals into non-overlapping intervals where
-- each resulting interval is the leaf in the hierarchy at any given time. The result also
-- denotes the 'self-time' of each interval.
--
-- Each interval is a (root_id, root_ts, root_dur, id, parent_id, ts, dur) and the overlap is
-- represented as a (root_id, id, parent_id, ts, dur).
-- Note that, children intervals must not be longer than any ancestor interval.
-- See _intervals_merge_root_and_children that can be used to generate input to this macro
-- from two different root and children tables.
CREATE PERFETTO MACRO _intervals_flatten(children_with_roots_table TableOrSubquery)
-- The returned table has the schema (root_id UINT32, id UINT32, ts INT64, dur INT64).
RETURNS TableOrSubquery
AS (
  -- Algorithm: Sort all the start and end timestamps of the children within a root.
  -- The interval duration between one timestamp and the next is one result.
  -- If the timestamp is a start, the id is the id of the interval, if it's an end,
  -- it's the parent_id.
  -- Special case the edges of the roots and roots without children.
  WITH
    _children_with_roots AS (
      SELECT * FROM ($children_with_roots_table) WHERE root_dur > 0 AND (dur IS NULL OR dur > 0)
    ),
    _ends AS (
      SELECT
        root_id,
        root_ts,
        root_dur,
        IFNULL(parent_id, root_id) AS id,
        ts + dur AS ts
      FROM _children_with_roots WHERE id IS NOT NULL
    ),
    _events AS (
      SELECT root_id, root_ts, root_dur, id, ts, 1 AS priority
      FROM _children_with_roots
      UNION ALL
      SELECT root_id, root_ts, root_dur, id, ts, 0 AS priority FROM _ends
    ),
    _events_deduped AS (
      SELECT root_id, root_ts, root_dur, id, ts
      FROM _events
      GROUP BY root_id, ts
      HAVING priority = MAX(priority)
    ),
    _intervals AS (
      SELECT
        root_id,
        root_ts,
        root_dur,
        id,
        ts,
        LEAD(ts)
          OVER (PARTITION BY root_id ORDER BY ts) - ts AS dur
      FROM _events_deduped
    ),
    _only_middle AS (
      SELECT * FROM _intervals WHERE dur > 0
    ),
    _only_start AS (
      SELECT
        root_id,
        root_id AS id,
        root_ts AS ts,
        MIN(ts) - root_ts AS dur
      FROM _only_middle
      GROUP BY root_id
      HAVING dur > 0
    ),
    _only_end AS (
      SELECT
        root_id,
        root_id AS id,
        MAX(ts + dur) AS ts,
        root_ts + root_dur - MAX(ts + dur) AS dur
      FROM _only_middle
      GROUP BY root_id
      HAVING dur > 0
    ),
    _only_singleton AS (
      SELECT root_id, root_id AS id, root_ts AS ts, root_dur AS dur
      FROM _children_with_roots WHERE id IS NULL
      GROUP BY root_id
    )
  SELECT root_id, id, ts, dur FROM _only_middle
  UNION ALL
  SELECT root_id, id, ts, dur FROM _only_start
  UNION ALL
  SELECT root_id, id, ts, dur FROM _only_end
  UNION ALL
  SELECT root_id, id, ts, dur FROM _only_singleton
);
