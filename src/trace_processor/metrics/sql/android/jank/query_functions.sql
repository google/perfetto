--
-- Copyright 2022 The Android Open Source Project
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


-- Consider calling ANDROID_JANK_CORRELATE_FRAME_SLICE which passes a default value for
-- `table_name_prefix`.
--
-- Matches slices with frames within CUJs and aggregates slices durations within each frame.
-- This allows comparing the cumulative durations of a set of slices vs what was the expected
-- duration of each frame. It can be a useful heuristic to figure out what contributed to
-- frames missing their expected deadlines.
--
-- For more details see the documentation in query_frame_slice.sql.
--
-- Example usage:
--
-- CREATE VIEW example_table AS
-- SELECT * FROM android_jank_cuj_slice WHERE name = 'binder transaction';
-- SELECT ANDROID_JANK_CORRELATE_FRAME_SLICE_IMPL('MainThread', 'example_table', 'jank_query');
-- SELECT * FROM jank_query_slice_in_frame_agg;
--
-- Function arguments:
--
-- table_set - Name of a set of tables from `android_jank_cuj_table_set`.
--             Groups releated tables to simplify passing them as arguments to functions.
--
-- relevant_slice_table_name - Table or View which selects slices for analysis from the
--                             `android_jank_cuj_slice` table.
--
-- table_name_prefix - Running the function will create multiple tables. This value will be used
--                     as a prefx for their names to avoid name collisions with other tables.
SELECT CREATE_FUNCTION(
  'ANDROID_JANK_CORRELATE_FRAME_SLICE_IMPL(table_set STRING, relevant_slice_table_name STRING, table_name_prefix STRING)',
  'STRING',
  '
      SELECT COALESCE( -- COALESCE to return the text with table names to the caller instead of NULL
          RUN_METRIC(
            "android/jank/internal/query_frame_slice.sql",
            "table_name_prefix", $table_name_prefix,
            "relevant_slice_table_name", $relevant_slice_table_name,
            "slice_table_name", (SELECT ANDROID_JANK_CUJ_TABLE_SET_SLICE($table_set)),
            "frame_boundary_table_name", (SELECT ANDROID_JANK_CUJ_TABLE_SET_FRAME_BOUNDARY($table_set)),
            "frame_table_name", (SELECT ANDROID_JANK_CUJ_TABLE_SET_FRAME($table_set))),
          "Query results in `" || $table_name_prefix || "_slice_in_frame_agg` and `" || $table_name_prefix || "_slice_in_frame`.")
   '
);


-- Provides a default value for table_name_prefix in ANDROID_JANK_CORRELATE_FRAME_SLICE_IMPL.
-- See documentation for ANDROID_JANK_CORRELATE_FRAME_SLICE_IMPL.
SELECT CREATE_FUNCTION(
  'ANDROID_JANK_CORRELATE_FRAME_SLICE(table_set STRING, relevant_slice_table_name STRING)',
  'STRING',
  'SELECT ANDROID_JANK_CORRELATE_FRAME_SLICE_IMPL($table_set, $relevant_slice_table_name, "jank_query")'
);
