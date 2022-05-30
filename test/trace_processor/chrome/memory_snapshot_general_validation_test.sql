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

SELECT
  (
    SELECT COUNT(*) FROM memory_snapshot
  ) AS total_snapshots,
  (
    SELECT COUNT(*) FROM process
  ) AS total_processes,
  (
    SELECT COUNT(*) FROM process_memory_snapshot
  ) AS total_process_snapshots,
  (
    SELECT COUNT(*) FROM memory_snapshot_node
  ) AS total_nodes,
  (
    SELECT COUNT(*) FROM memory_snapshot_edge
  ) AS total_edges,
  (
    SELECT COUNT(DISTINCT args.id)
    FROM args
    INNER JOIN memory_snapshot_node
    ON args.arg_set_id = memory_snapshot_node.arg_set_id
  ) AS total_node_args,
  (
    SELECT COUNT(*) FROM profiler_smaps
    INNER JOIN memory_snapshot ON timestamp = ts
  ) AS total_smaps
