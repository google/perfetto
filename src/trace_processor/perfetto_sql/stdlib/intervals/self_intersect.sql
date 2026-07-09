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

-- sqlformat file off

-- Self-intersection of an interval table. Identical output contract to the
-- existing intervals.intersect.interval_self_intersect SQL macro — drop-in
-- replacement, implemented via the C++ plugin
-- plugins/interval_self_intersect (single-pass O(n log n) sweep).
--
-- For each atomic time segment defined by the endpoints of |intervals| and
-- for every interval active in that segment, emits one row with the
-- interval's id and interval_ends_at_ts = 0. For each interval, also emits
-- one "end marker" row at the segment that begins at the interval's end ts
-- with interval_ends_at_ts = 1; the final endpoint produces a dur=0
-- segment containing only end markers (matching the SQL macro's quirk).
--
-- |intervals| must expose `id INT64`, `ts INT64`, `dur INT64`.
-- Output:
--   ts INT64                start of the atomic segment
--   dur INT64               duration to the next endpoint (0 at the final)
--   group_id INT64          1-indexed stable per-segment id
--   id INT64                original interval id
--   interval_ends_at_ts INT64    0 = active in segment, 1 = end marker
CREATE PERFETTO MACRO _interval_self_intersect(
  intervals TableOrSubquery
)
RETURNS TableOrSubquery AS
(
  SELECT
    c0 AS ts,
    c1 AS dur,
    c2 AS group_id,
    c3 AS id,
    c4 AS interval_ends_at_ts
  FROM __intrinsic_table_ptr(
    __intrinsic_interval_self_intersect(
      (
        SELECT
          __intrinsic_interval_tree_intervals_agg(
            input.id, input.ts, input.dur
          )
        FROM (SELECT * FROM $intervals ORDER BY ts) input
      )
    )
  )
  WHERE __intrinsic_table_ptr_bind(c0, 'ts')
    AND __intrinsic_table_ptr_bind(c1, 'dur')
    AND __intrinsic_table_ptr_bind(c2, 'group_id')
    AND __intrinsic_table_ptr_bind(c3, 'id')
    AND __intrinsic_table_ptr_bind(c4, 'interval_ends_at_ts')
);
