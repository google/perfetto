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

INCLUDE PERFETTO MODULE linux.threads;

INCLUDE PERFETTO MODULE viz.summary.trace;

-- Create a new table containing the utids of all the kernel threads.
-- Right now this table only supports linux traces. On all other traces the
-- resultant table will be empty.
CREATE PERFETTO PIPELINE _kernel_threads MATERIALIZED AS
FROM linux_kernel_threads
|> WHERE _is_linux_machine(machine_id) = 1
|> SELECT utid;

CREATE PERFETTO PIPELINE _threads_with_kernel_flag MATERIALIZED AS
SUBPIPELINE kernel AS (
  FROM _kernel_threads
)
FROM thread
|> SELECT
  id,
  utid,
  upid,
  tid,
  name,
  is_main_thread,
  is_idle,
  machine_id,
  utid IN (SELECT utid FROM kernel) AS is_kernel_thread;
