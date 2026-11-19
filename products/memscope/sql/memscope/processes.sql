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

-- Processes with memory captures (Java heap dumps, smaps snapshots or native
-- heap profiles), scored by how much there is to analyse. Memscope picks the
-- highest scoring process by default.
CREATE PERFETTO TABLE memscope_process_mem_stats(
  -- Process id.
  upid JOINID(process.id),
  -- Pid of the process.
  pid LONG,
  -- Cmdline, or name, of the process.
  process_name STRING,
  -- Number of Java heap dumps.
  heap_dumps LONG,
  -- Number of distinct smaps snapshots.
  smaps_snapshots LONG,
  -- Number of distinct native heap profile dumps.
  native_dumps LONG,
  -- Relevance score: heap dumps weigh most, then smaps, then native dumps.
  score LONG
)
AS
WITH
  heap AS (SELECT upid, count(*) AS n FROM heap_graph GROUP BY upid),
  smaps AS (
    SELECT upid, count(DISTINCT ts) AS n FROM profiler_smaps GROUP BY upid
  ),
  native AS (
    SELECT upid, count(DISTINCT ts) AS n
    FROM heap_profile_allocation
    GROUP BY
      upid
  )
SELECT
  p.upid,
  p.pid,
  coalesce(p.cmdline, p.name, '<unknown>') AS process_name,
  coalesce(heap.n, 0) AS heap_dumps,
  coalesce(smaps.n, 0) AS smaps_snapshots,
  coalesce(native.n, 0) AS native_dumps,
  coalesce(heap.n, 0) * 3 + coalesce(smaps.n, 0) * 2 + coalesce(native.n, 0) AS score
FROM process AS p
LEFT JOIN heap USING (upid)
LEFT JOIN smaps USING (upid)
LEFT JOIN native USING (upid)
WHERE
  heap.n > 0
  OR smaps.n > 0
  OR native.n > 0
ORDER BY
  p.upid;

-- Returns the process Memscope analyses by default: the highest scoring one,
-- lowest upid first on ties.
CREATE PERFETTO FUNCTION memscope_best_process()
-- upid of the process, or NULL if no process has memory captures.
RETURNS LONG
AS
SELECT upid
FROM memscope_process_mem_stats
ORDER BY
  score DESC,
  upid ASC
LIMIT 1;
