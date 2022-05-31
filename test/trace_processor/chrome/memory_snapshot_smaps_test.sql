--
-- Copyright 2020 The Android Open Source Project
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

SELECT
  process.upid,
  process.name,
  smap.ts,
  path,
  size_kb,
  private_dirty_kb,
  swap_kb,
  file_name,
  start_address,
  module_timestamp,
  module_debugid,
  module_debug_path,
  protection_flags,
  private_clean_resident_kb,
  shared_dirty_resident_kb,
  shared_clean_resident_kb,
  locked_kb,
  proportional_resident_kb
FROM process
INNER JOIN profiler_smaps smap ON process.upid = smap.upid
INNER JOIN memory_snapshot ms ON ms.timestamp = smap.ts
LIMIT 20
