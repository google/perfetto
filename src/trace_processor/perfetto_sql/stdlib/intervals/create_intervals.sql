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

-- Given a table of start timestamps and a table of end timestamps, creates
-- intervals by matching each start with the next end timestamp strictly greater
-- than it.
--
-- Both input tables must have columns named `id` and `ts`. Uses an efficient
-- O(n+m) two-pointer algorithm implemented in C++.
--
-- Example:
-- ```
-- SELECT * FROM _interval_create!(
--   (SELECT id, ts FROM starts_table),
--   (SELECT id, ts FROM ends_table)
-- )
-- ```
CREATE PERFETTO MACRO _interval_create(
    -- Table or subquery containing start timestamps (must have `id` and `ts`
    -- columns).
    starts_table TableOrSubquery,
    -- Table or subquery containing end timestamps (must have `id` and `ts`
    -- columns).
    ends_table TableOrSubquery
)
-- Table with the schema:
-- ts TIMESTAMP,
--     The start timestamp.
-- dur DURATION,
--     The duration from start to the matched end.
-- start_id LONG,
--     The id of the matched start row.
-- end_id LONG,
--     The id of the matched end row.
RETURNS TableOrSubquery AS
(
  SELECT
    c0 AS ts,
    c1 AS dur,
    c2 AS start_id,
    c3 AS end_id
  FROM __intrinsic_table_ptr(
    __intrinsic_interval_create(
      (
        SELECT
          __intrinsic_timestamp_set_agg(ordered_s.id, ordered_s.ts)
        FROM (
          SELECT
            id,
            ts
          FROM $starts_table
          ORDER BY
            ts
        ) AS ordered_s
      ),
      (
        SELECT
          __intrinsic_timestamp_set_agg(ordered_e.id, ordered_e.ts)
        FROM (
          SELECT
            id,
            ts
          FROM $ends_table
          ORDER BY
            ts
        ) AS ordered_e
      )
    )
  )
  WHERE
    __intrinsic_table_ptr_bind(c0, 'ts')
    AND __intrinsic_table_ptr_bind(c1, 'dur')
    AND __intrinsic_table_ptr_bind(c2, 'start_id')
    AND __intrinsic_table_ptr_bind(c3, 'end_id')
);
