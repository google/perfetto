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

SELECT RUN_METRIC('android/global_counter_span_view.sql',
  'table_name', 'global_gpu_memory',
  'counter_name', 'GPU Memory');

SELECT RUN_METRIC('android/process_counter_span_view.sql',
  'table_name', 'proc_gpu_memory',
  'counter_name', 'GPU Memory');

CREATE VIEW proc_gpu_memory_view AS
SELECT
  upid,
  CAST(MAX(proc_gpu_memory_val) as INT64) as mem_max,
  CAST(MIN(proc_gpu_memory_val) as INT64) as mem_min,
  CAST(SUM(proc_gpu_memory_val * dur) / SUM(dur) as INT64) as mem_avg
FROM proc_gpu_memory_span
GROUP BY upid;

CREATE VIEW proc_gpu_view AS
SELECT
  AndroidGpuMetric_Process(
    'name', p.name,
    'mem_max', v.mem_max,
    'mem_min', v.mem_min,
    'mem_avg', v.mem_avg
  ) AS proto
FROM process p
JOIN proc_gpu_memory_view v
ON p.upid = v.upid;

CREATE VIEW android_gpu_output AS
SELECT AndroidGpuMetric(
  'processes', (SELECT RepeatedField(proto) FROM proc_gpu_view),
  'mem_max', CAST(MAX(global_gpu_memory_val) as INT64),
  'mem_min', CAST(MIN(global_gpu_memory_val) as INT64),
  'mem_avg', CAST(SUM(global_gpu_memory_val * dur) / SUM(dur) as INT64)
)
FROM global_gpu_memory_span;
