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
  MAX(proc_gpu_memory_val) as mem_max,
  MIN(proc_gpu_memory_val) as mem_min,
  SUM(proc_gpu_memory_val * dur) as mem_valxdur,
  SUM(dur) as mem_dur
FROM proc_gpu_memory_span
GROUP BY upid;

CREATE VIEW agg_proc_gpu_view AS
SELECT
  name,
  MAX(mem_max) as mem_max,
  MIN(mem_min) as mem_min,
  SUM(mem_valxdur) / SUM(mem_dur) as mem_avg
FROM process
JOIN proc_gpu_memory_view
USING(upid)
GROUP BY name;

CREATE VIEW proc_gpu_view AS
SELECT
  AndroidGpuMetric_Process(
    'name', name,
    'mem_max', CAST(mem_max as INT64),
    'mem_min', CAST(mem_min as INT64),
    'mem_avg', CAST(mem_avg as INT64)
  ) AS proto
FROM agg_proc_gpu_view;

CREATE VIEW android_gpu_output AS
SELECT AndroidGpuMetric(
  'processes', (SELECT RepeatedField(proto) FROM proc_gpu_view),
  'mem_max', CAST(MAX(global_gpu_memory_val) as INT64),
  'mem_min', CAST(MIN(global_gpu_memory_val) as INT64),
  'mem_avg', CAST(SUM(global_gpu_memory_val * dur) / SUM(dur) as INT64)
)
FROM global_gpu_memory_span;
