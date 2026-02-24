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
-- slices by matching each start with the next end timestamp strictly greater
-- than it.
--
-- Uses an efficient O(n+m) two-pointer algorithm implemented in C++.
--
-- Example:
-- ```
-- SELECT * FROM _create_slices!(
--   (SELECT ts FROM starts_table),
--   (SELECT ts FROM ends_table),
--   ts,
--   ts
-- )
-- ```
CREATE PERFETTO MACRO _create_slices(
    -- Table or subquery containing start timestamps.
    starts_table TableOrSubquery,
    -- Table or subquery containing end timestamps.
    ends_table TableOrSubquery,
    -- Name of the timestamp column in the starts table.
    starts_ts_col ColumnName,
    -- Name of the timestamp column in the ends table.
    ends_ts_col ColumnName
)
-- Table with the schema:
-- ts TIMESTAMP,
--     The start timestamp.
-- dur DURATION,
--     The duration from start to the matched end.
RETURNS TableOrSubquery AS
(
  SELECT
    c0 AS ts,
    c1 AS dur
  FROM __intrinsic_table_ptr(
    __intrinsic_create_slices(
      (
        SELECT
          __intrinsic_timestamp_set_agg(s.$starts_ts_col)
        FROM $starts_table AS s
      ),
      (
        SELECT
          __intrinsic_timestamp_set_agg(e.$ends_ts_col)
        FROM $ends_table AS e
      )
    )
  )
  WHERE
    __intrinsic_table_ptr_bind(c0, 'ts') AND __intrinsic_table_ptr_bind(c1, 'dur')
);
