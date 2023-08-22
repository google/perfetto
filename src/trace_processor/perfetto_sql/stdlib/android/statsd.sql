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
-- @column id                        Unique identifier for this slice.
-- @column type                      The name of the "most-specific" child table containing this row.
-- @column ts                        The timestamp at the start of the slice (in nanoseconds).
-- @column dur                       The duration of the slice (in nanoseconds).
-- @column arg_set_id                The id of the argument set associated with this slice.
-- @column thread_instruction_count  The value of the CPU instruction counter at the start of the slice. This column will only be populated if thread instruction collection is enabled with track_event.
-- @column thread_instruction_delta  The change in value of the CPU instruction counter between the start and end of the slice. This column will only be populated if thread instruction collection is enabled with track_event.
-- @column track_id                  The id of the track this slice is located on.
-- @column category                  The "category" of the slice. If this slice originated with track_event, this column contains the category emitted. Otherwise, it is likely to be null (with limited exceptions).
-- @column name                      The name of the slice. The name describes what was happening during the slice.
-- @column depth                     The depth of the slice in the current stack of slices.
-- @column stack_id                  A unique identifier obtained from the names of all slices in this stack. This is rarely useful and kept around only for legacy reasons.
-- @column parent_stack_id           The stack_id for the parent of this slice. Rarely useful.
-- @column parent_id                 The id of the parent (i.e. immediate ancestor) slice for this slice.
-- @column thread_ts                 The thread timestamp at the start of the slice. This column will only be populated if thread timestamp collection is enabled with track_event.
-- @column thread_dur                The thread time used by this slice. This column will only be populated if thread timestamp collection is enabled with track_event.
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


