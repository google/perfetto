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

CREATE PERFETTO MACRO _interval_intersect(
  left_table TableOrSubquery,
  right_table TableOrSubquery
)
RETURNS TableOrSubquery AS
(
  WITH
    __temp_left_table AS (SELECT * FROM $left_table ORDER BY ts),
    __temp_right_table AS (SELECT * FROM $right_table ORDER BY ts)
  SELECT ii.ts, ii.dur, ii.left_id, ii.right_id
  FROM __intrinsic_interval_intersect(
    (SELECT RepeatedField(id) FROM __temp_left_table),
    (SELECT RepeatedField(ts) FROM __temp_left_table),
    (SELECT RepeatedField(dur) FROM __temp_left_table),
    (SELECT RepeatedField(id) FROM __temp_right_table),
    (SELECT RepeatedField(ts) FROM __temp_right_table),
    (SELECT RepeatedField(dur) FROM __temp_right_table)
  ) ii
);

CREATE PERFETTO MACRO _interval_intersect_single(
  ts Expr,
  dur Expr,
  intervals_table TableOrSubquery
) RETURNS TableOrSubquery AS (
  SELECT
    left_id AS id,
    ts,
    dur
  FROM _interval_intersect!(
    $intervals_table,
    (SELECT
        0 AS id,
        $ts AS ts,
        $dur AS dur
    )
  )
)
