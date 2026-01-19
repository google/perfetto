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

INCLUDE PERFETTO MODULE intervals.intersect;

-- ------------------------------------------------------------------
-- Global constants and bucket count
-- ------------------------------------------------------------------
CREATE PERFETTO MACRO _mm_bucket_metadata(
    -- The source table containing ts and dur columns
    _source_table TableOrSubQuery,
    -- The bucket duration in nanoseconds
    _bucket_duration_ns Expr
)
RETURNS TableOrSubQuery AS
(
  WITH
    mm_constants AS (
      SELECT
        $_bucket_duration_ns AS bucket_duration_ns,
        (
          SELECT
            min(ts)
          FROM $_source_table
        ) AS trace_min_ts,
        (
          SELECT
            max(ts + dur)
          FROM $_source_table
        ) AS trace_max_ts
    ),
    mm_span AS (
      SELECT
        trace_min_ts,
        trace_max_ts,
        bucket_duration_ns,
        (
          trace_max_ts - trace_min_ts
        ) AS total_span
      FROM mm_constants
    )
  SELECT
    trace_min_ts,
    trace_max_ts,
    bucket_duration_ns,
    (
      (
        total_span + bucket_duration_ns - 1
      ) / bucket_duration_ns
    ) AS bucket_count
  FROM mm_span
);

-- Creates the buckets table
CREATE PERFETTO MACRO _mm_buckets_table(
    -- The source table containing ts and dur columns
    _source_table TableOrSubQuery,
    -- The bucket duration in nanoseconds
    _bucket_duration_ns Expr
)
RETURNS TableOrSubQuery AS
(
  WITH
  RECURSIVE   bucket_meta AS (
      SELECT
        trace_min_ts,
        trace_max_ts,
        bucket_duration_ns,
        bucket_count
      FROM _mm_bucket_metadata!($_source_table, $_bucket_duration_ns)
    ),
    buckets(id, bucket_index, ts, dur) AS (
      SELECT
        0 AS id,
        0 AS bucket_index,
        bm.trace_min_ts AS ts,
        bm.bucket_duration_ns AS dur
      FROM bucket_meta AS bm
      UNION ALL
      SELECT
        id + 1,
        bucket_index + 1,
        ts + bm.bucket_duration_ns,
        bm.bucket_duration_ns
      FROM buckets, bucket_meta AS bm
      WHERE
        bucket_index + 1 < bm.bucket_count
    )
  SELECT
    *
  FROM buckets
);

-- Startup specific mipmap calculation
CREATE PERFETTO MACRO _mm_merged(
    -- The source table containing id, ts, dur and a group hash column for merging
    _source_slices_with_ids TableOrSubQuery,
    -- The buckets table containing id, ts, dur columns
    _buckets TableOrSubQuery,
    -- The bucket duration in nanoseconds
    _bucket_duration_ns Expr
)
RETURNS TableOrSubQuery AS
(
  WITH
    mm_intersections AS (
      WITH
        mm_ii AS (
          SELECT
            *
          FROM _interval_intersect !(
          ($_buckets, $_source_slices_with_ids),
          ()
        )
        )
      SELECT
        ii.ts AS overlap_ts,
        ii.dur AS overlap_dur,
        b.bucket_index,
        b.ts AS bucket_ts,
        b.dur AS bucket_dur,
        s.id AS source_id,
        s.group_hash,
        s.ts AS original_ts,
        s.dur AS original_dur
      FROM mm_ii AS ii
      JOIN $_buckets AS b
        ON b.id = id_0
      JOIN $_source_slices_with_ids AS s
        ON s.id = id_1
    ),
    -- Sum total overlap per bucket
    mm_bucket_aggregates AS (
      SELECT
        bucket_index,
        bucket_ts AS ts,
        sum(overlap_dur) AS total_bucket_dur
      FROM mm_intersections
      GROUP BY
        bucket_index,
        ts
    ),
    -- Sum total overlap per source slice per bucket
    mm_source_bucket_aggregates AS (
      SELECT
        bucket_index,
        group_hash,
        -- in case of multiple ids for same group_hash
        min(source_id) AS source_id,
        sum(overlap_dur) AS total_overlap_dur,
        min(original_ts) AS min_original_ts
      FROM mm_intersections
      GROUP BY
        bucket_index,
        group_hash
    ),
    -- Pick the dominant event per bucket by max overlap (then tie-break).
    mm_ranked AS (
      SELECT
        bucket_index,
        source_id,
        group_hash,
        total_overlap_dur,
        row_number() OVER (PARTITION BY bucket_index ORDER BY total_overlap_dur DESC, min_original_ts ASC, group_hash ASC) AS rn
      FROM mm_source_bucket_aggregates
    ),
    mm_dominant_events AS (
      SELECT
        bucket_index,
        source_id,
        group_hash
      FROM mm_ranked
      WHERE
        rn = 1
    ),
    -- Attach total overlap to the dominant label to build a per-bucket row.
    mm_unmerged_buckets AS (
      SELECT
        ba.bucket_index,
        ba.ts,
        ba.total_bucket_dur AS dur,
        de.source_id,
        de.group_hash
      FROM mm_bucket_aggregates AS ba
      JOIN mm_dominant_events AS de
        USING (bucket_index)
    ),
    -- Merge contiguous buckets where dominant group_hash is the same.
    mm_with_changes AS (
      SELECT
        *,
        group_hash != coalesce(lag(group_hash) OVER (ORDER BY bucket_index), '') AS is_change
      FROM mm_unmerged_buckets
    ),
    mm_with_islands AS (
      SELECT
        *,
        sum(CAST(is_change AS INTEGER)) OVER (ORDER BY bucket_index ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) AS island_id
      FROM mm_with_changes
    )
  SELECT
    -- an island could multiple source ids
    min(source_id) AS id,
    cast_int!(min(ts)) AS ts,
    (
      max(bucket_index) - min(bucket_index) + 1
    ) * $_bucket_duration_ns AS dur,
    group_hash
  FROM mm_with_islands
  GROUP BY
    island_id,
    group_hash
  ORDER BY
    ts
);
