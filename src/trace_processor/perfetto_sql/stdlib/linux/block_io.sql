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

-- View tracking the number of IO operations remaining in the kernel IO queue or
-- a block device
CREATE PERFETTO PIPELINE linux_active_block_io_operations_by_device(
  -- timestamp when block_io_start or block_io_done happened
  ts LONG,
  -- the number of IO operations in the kernel queue or the device
  ops_in_queue_or_device LONG,
  -- the device processing the IO operations
  dev LONG
) AS
FROM slice
|> JOIN track ON slice.track_id = track.id AND track.type = 'block_io'
|> SELECT
     slice.ts AS ts,
     slice.dur AS dur,
     extract_arg(track.dimension_arg_set_id, 'block_device') AS dev
|> INTERVAL FLATTEN PER dev AGGREGATE COUNT(*) AS ops_in_queue_or_device
|> SELECT ts, ops_in_queue_or_device, dev;
