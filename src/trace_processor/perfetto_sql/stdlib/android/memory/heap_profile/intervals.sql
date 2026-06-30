--
-- Copyright 2024 The Android Open Source Project
--
-- Licensed under the Apache License, Version 2.0 (the 'License');
-- you may not use this file except in compliance with the License.
-- You may obtain a copy of the License at
--
--     https://www.apache.org/licenses/LICENSE-2.0
--
-- Unless required by applicable law or agreed to in writing, software
-- distributed under the License is distributed on an 'AS IS' BASIS,
-- WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
-- See the License for the specific language governing permissions and
-- limitations under the License.

-- One row per (process, heap, dump) describing the dump as a slice spanning its
-- profiling interval.
--
-- The interval end is the dump timestamp (heap_profile.ts_end, which is also
-- the heap_profile_allocation timestamp). The start is heap_profile.ts when the
-- producer recorded it. For traces from older producers (which only emit the
-- dump timestamp, so heap_profile.ts == ts_end) we fall back to the previous
-- dump for the same heap, or the trace start for the first dump - matching the
-- behaviour the UI relied on before start timestamps existed.
CREATE PERFETTO TABLE _android_heap_profile_intervals AS
WITH
  dumps AS (
    SELECT
      min(a.id) AS id,
      a.upid AS upid,
      a.heap_name AS heap_name,
      a.ts AS ts_end,
      hp.ts AS start_ts
    FROM heap_profile_allocation AS a
    JOIN heap_profile AS hp ON hp.upid = a.upid AND a.ts = hp.ts_end
    GROUP BY a.upid, a.heap_name, a.ts
  ),
  with_start AS (
    SELECT
      id,
      upid,
      heap_name,
      ts_end,
      iif(
        start_ts < ts_end,
        start_ts,
        -- Clamp to ts_end so the very first dump (which has no previous dump and
        -- falls back to the trace start) never produces a negative duration.
        min(
          lag(ts_end, 1, trace_start()) OVER (
            PARTITION BY upid, heap_name ORDER BY ts_end
          ) + 1,
          ts_end
        )
      ) AS ts
    FROM dumps
  )
SELECT
  id,
  upid,
  heap_name,
  ts,
  ts_end - ts AS dur
FROM with_start;
