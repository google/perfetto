--
-- Copyright 2019 The Android Open Source Project
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

-- Create all the views used to generate the Android Cpu metrics proto.
SELECT RUN_METRIC('android/android_cpu_agg.sql');

CREATE VIEW cpu_breakdown_per_utid AS
SELECT
  utid,
  AndroidCpuMetric_CpuFrequencyData(
    'id', cpu,
    'avg_freq_khz', CAST((SUM(dur * freq) / SUM(dur)) AS INT),
    'duration_ns', CAST(SUM(dur) AS INT),
    'min_freq_khz', CAST(MIN(freq) AS INT),
    'max_freq_khz', CAST(MAX(freq) AS INT)
  ) as cpu_freq_proto
FROM cpu_freq_sched_per_thread
GROUP BY utid, cpu;

-- CPU info aggregated per thread, with repeated containing separate data for
-- each CPU.
CREATE VIEW agg_by_thread AS
SELECT
  utid,
  upid,
  thread.name AS thread_name,
  process.name AS process_name,
  RepeatedField(cpu_freq_proto) AS thread_proto_cpu
FROM thread
JOIN process USING (upid)
LEFT JOIN cpu_breakdown_per_utid USING (utid)
GROUP BY utid;

-- CPU info aggregated per process, with repeated containing separate data for
-- thread.
CREATE TABLE agg_by_process AS
SELECT
  upid,
  process_name,
  RepeatedField(
    AndroidCpuMetric_Thread(
      'name', CAST(thread_name as TEXT),
      'cpu', thread_proto_cpu
    )
  ) AS thread_proto
FROM agg_by_thread
GROUP BY upid;

-- Generate Process proto.
CREATE VIEW thread_cpu_view AS
SELECT
  AndroidCpuMetric_Process(
    'name', process_name,
    'threads', thread_proto
  ) AS cpu_info_process
FROM agg_by_process
GROUP BY upid;

CREATE VIEW android_cpu_output AS
SELECT AndroidCpuMetric(
  'process_info', (
    SELECT RepeatedField(cpu_info_process) FROM thread_cpu_view
  )
);
