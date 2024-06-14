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

-- Partition and flatten a hierarchy of intervals into non-overlapping intervals where
-- each resulting interval is the leaf in the hierarchy at any given time. The result also
-- denotes the 'self-time' of each interval.
--
-- Each interval is a (root_id, id, parent_id, ts, dur) and the overlap is also represented as a
-- (root_id, id, parent_id, ts, dur).
-- Note that, children intervals must not be longer than any ancestor interval.
CREATE PERFETTO MACRO _intervals_flatten(
  -- Table or subquery containing all the root intervals: (id, ts, dur). Note that parent_id
  -- is not necessary in this table as it will be NULL anyways.
  roots_table TableOrSubquery,
  -- Table or subquery containing all the child intervals. (root_id, id, parent_id, ts, dur)
  children_table TableOrSubquery)
RETURNS TableOrSubquery
  AS (
    -- Algorithm: Sort all the start and end timestamps of the children within a root.
    -- The interval duration between one timestamp and the next is one result.
    -- If the timestamp is a start, the id is the id of the interval, if it's an end,
    -- it's the parent_id.
    -- Special case the edges of the roots and roots without children.
  WITH
    _roots AS (
      SELECT * FROM ($roots_table) WHERE dur > 0
    ),
    _children AS (
      SELECT * FROM ($children_table) WHERE dur > 0
    ),
    _roots_without_children AS (
      SELECT id FROM _roots
      EXCEPT
      SELECT DISTINCT parent_id AS id FROM _children
    ),
    _children_with_root_ts_and_dur AS (
      SELECT
        _roots.id AS root_id,
        _roots.ts AS root_ts,
        _roots.dur AS root_dur,
        _children.id,
        _children.parent_id,
        _children.ts,
        _children.dur
      FROM _children
      JOIN _roots ON _roots.id = root_id
    ),
    _ends AS (
      SELECT
        child.root_id,
        child.root_ts,
        child.root_dur,
        IFNULL(parent.id, child.root_id) AS id,
        parent.parent_id,
        child.ts + child.dur AS ts
      FROM _children_with_root_ts_and_dur child
      LEFT JOIN _children_with_root_ts_and_dur parent
        ON child.parent_id = parent.id
    ),
    _events AS (
      SELECT root_id, root_ts, root_dur, id, parent_id, ts FROM _children_with_root_ts_and_dur
      UNION ALL
      SELECT root_id, root_ts, root_dur, id, parent_id, ts FROM _ends
    ),
    _intervals AS (
      SELECT
        root_id,
        root_ts,
        root_dur,
        id,
        parent_id,
        ts,
        LEAD(ts)
          OVER (PARTITION BY root_id ORDER BY ts) - ts AS dur
      FROM _events
    ),
    _only_middle AS (
      SELECT * FROM _intervals WHERE dur > 0
    ),
    _only_start AS (
      SELECT
        root_id,
        parent_id AS id,
        NULL AS parent_id,
        root_ts AS ts,
        MIN(ts) - root_ts AS dur
      FROM _only_middle
      GROUP BY root_id
    ),
    _only_end AS (
      SELECT
        root_id,
        parent_id AS id,
        NULL AS parent_id,
        MAX(ts + dur) AS ts,
        root_ts + root_dur - MAX(ts + dur) AS dur
      FROM _only_middle
      GROUP BY root_id
    ),
    _only_singleton AS (
      SELECT id AS root_id, id, NULL AS parent_id, ts, dur
      FROM _roots
      JOIN _roots_without_children USING (id)
    )
  SELECT root_id, id, parent_id, ts, dur FROM _only_middle
  UNION ALL
  SELECT root_id, id, parent_id, ts, dur FROM _only_start
  UNION ALL
  SELECT root_id, id, parent_id, ts, dur FROM _only_end
  UNION ALL
  SELECT root_id, id, parent_id, ts, dur FROM _only_singleton
);
