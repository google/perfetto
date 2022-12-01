--
-- Copyright 2021 The Android Open Source Project
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

DROP VIEW IF EXISTS dma_heap_timeline;
CREATE VIEW dma_heap_timeline AS
SELECT
  ts,
  LEAD(ts, 1, (SELECT end_ts FROM trace_bounds))
  OVER(PARTITION BY track_id ORDER BY ts) - ts AS dur,
  track_id,
  value
FROM counter JOIN counter_track
  ON counter.track_id = counter_track.id
WHERE (name = 'mem.dma_heap');

DROP VIEW IF EXISTS dma_heap_stats;
CREATE VIEW dma_heap_stats AS
SELECT
  SUM(value * dur) / SUM(dur) AS avg_size,
  MIN(value) AS min_size,
  MAX(value) AS max_size
FROM dma_heap_timeline;

DROP VIEW IF EXISTS dma_heap_raw_allocs;
CREATE VIEW dma_heap_raw_allocs AS
SELECT
  ts,
  value AS instant_value,
  SUM(value) OVER win AS value
FROM counter c JOIN thread_counter_track t ON c.track_id = t.id
WHERE (name = 'mem.dma_heap_change') AND value > 0
WINDOW win AS (
  PARTITION BY name ORDER BY ts
  ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW
);

DROP VIEW IF EXISTS dma_heap_total_stats;
CREATE VIEW dma_heap_total_stats AS
SELECT
  SUM(instant_value) AS total_alloc_size_bytes
FROM dma_heap_raw_allocs;

-- We need to group by ts here as we can have two ion events from
-- different processes occurring at the same timestamp. We take the
-- max as this will take both allocations into account at that
-- timestamp.
DROP VIEW IF EXISTS android_dma_heap_event;
CREATE VIEW android_dma_heap_event AS
SELECT
  'counter' AS track_type,
  printf('Buffers created from DMA-BUF heaps: ') AS track_name,
  ts,
  MAX(value) AS value
FROM dma_heap_raw_allocs
GROUP BY 1, 2, 3;

DROP VIEW IF EXISTS android_dma_heap_output;
CREATE VIEW android_dma_heap_output AS
SELECT AndroidDmaHeapMetric(
  'avg_size_bytes', avg_size,
  'min_size_bytes', min_size,
  'max_size_bytes', max_size,
  'total_alloc_size_bytes', total_alloc_size_bytes
  )
FROM dma_heap_stats JOIN dma_heap_total_stats;
