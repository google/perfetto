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

-- Formats slices into JSON.
CREATE PERFETTO MACRO _format_slice_json(
    slice_table TableOrSubquery,
    group_col ColumnName
)
RETURNS TableOrSubquery AS
(
  SELECT
    $group_col,
    json_object(
      'name', name,
      'cat', 'slice',
      'ph', 'X',
      'ts', ts / 1000,
      'dur', dur / 1000,
      'pid', upid,
      'tid', utid,
      'args', json_object('depth', depth)
    ) AS event_json
  FROM $slice_table
);

-- Formats thread states into JSON.
CREATE PERFETTO MACRO _format_state_json(
    state_table TableOrSubquery,
    group_col ColumnName
)
RETURNS TableOrSubquery AS
(
  SELECT
    $group_col,
    json_object(
      'name', state,
      'cat', 'thread_state',
      'ph', 'T',
      'ts', ts / 1000,
      'dur', dur / 1000,
      'pid', upid,
      'tid', utid,
      'args', json_remove(
        json_object('blocked_function', blocked_function, 'cpu', cpu, 'io_wait', io_wait),
        CASE WHEN blocked_function IS NULL THEN '$.blocked_function' ELSE '$._none_' END,
        CASE WHEN cpu IS NULL THEN '$.cpu' ELSE '$._none_' END,
        CASE WHEN io_wait IS NULL THEN '$.io_wait' ELSE '$._none_' END
      )
    ) AS event_json
  FROM $state_table
);

-- Formats counters into JSON.
CREATE PERFETTO MACRO _format_counter_json(
    counter_table TableOrSubquery,
    group_col ColumnName
)
RETURNS TableOrSubquery AS
(
  SELECT
    $group_col,
    json_object(
      'name', name,
      'ph', 'C',
      'ts', ts / 1000,
      'pid', upid,
      'args', json_object(name, cast_double!(value))
    ) AS event_json
  FROM $counter_table
);
