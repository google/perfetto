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
--

-- Statsd atoms.
--
-- A subset of the slice table containing statsd atom instant events.
--
-- @column id,
-- @column type,
-- @column ts,
-- @column dur,
-- @column arg_set_id,
-- @column thread_instruction_count,
-- @column thread_instruction_delta,
-- @column track_id,
-- @column category,
-- @column name,
-- @column depth,
-- @column stack_id,
-- @column parent_stack_id,
-- @column parent_id,
-- @column thread_ts,
-- @column thread_dur,
CREATE VIEW android_statsd_atoms AS
SELECT
  slice.id AS id,
  slice.type AS type,
  slice.ts AS ts,
  slice.dur AS dur,
  slice.arg_set_id AS arg_set_id,
  slice.thread_instruction_count AS thread_instruction_count,
  slice.thread_instruction_delta AS thread_instruction_delta,
  slice.track_id AS track_id,
  slice.category AS category,
  slice.name AS name,
  slice.depth AS depth,
  slice.stack_id AS stack_id,
  slice.parent_stack_id AS parent_stack_id,
  slice.parent_id AS parent_id,
  slice.thread_ts AS thread_ts,
  slice.thread_dur AS thread_dur
FROM slice
JOIN track ON slice.track_id = track.id
WHERE
  track.name = 'Statsd Atoms';


