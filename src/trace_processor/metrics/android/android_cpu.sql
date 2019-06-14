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

-- CPU info aggregated per CPU and thread.
CREATE VIEW cpu_per_thread AS
SELECT utid,
       upid,
       cpu,
       CAST(SUM(duration) AS INT) AS duration,
       CAST(MIN(freq) AS INT) AS min_freq,
       CAST(MAX(freq) AS INT) AS max_freq,
       CAST((SUM(duration * freq) / SUM(duration)) AS INT) AS avg_frequency,
       process_name,
       thread_name
FROM (SELECT (MIN(cpu_freq_view.end_ts, thread_view.ts_end) - MAX(cpu_freq_view.start_ts, thread_view.ts)) AS duration,
             freq,
             thread_view.cpu as cpu,
             utid,
             upid,
             process_name,
             thread_name
      FROM cpu_freq_view JOIN thread_view ON(cpu_freq_view.cpu_id = thread_view.cpu)
      WHERE cpu_freq_view.start_ts < thread_view.ts_end AND cpu_freq_view.end_ts > thread_view.ts
      )
GROUP BY utid, cpu;

-- CPU info aggregated per thread, with repeated containing separate data for each CPU.
CREATE TABLE agg_by_thread AS
WITH RECURSIVE utid AS (SELECT DISTINCT utid FROM cpu_per_thread)
SELECT utid,
       upid,
       thread_name,
       process_name,
       cpu,
       RepeatedField(AndroidCpuMetric_CpuFrequencyData(
                             'id', cpu,
                             'avg_freq_khz', avg_frequency,
                             'duration_ns', duration,
                             'min_freq_khz', min_freq,
                             'max_freq_khz', max_freq)) AS thread_proto_cpu
FROM cpu_per_thread
GROUP BY utid;

-- CPU info aggregated per process, with repeated containing separate data for thread.
CREATE VIEW agg_by_process AS
WITH RECURSIVE upid AS (SELECT DISTINCT upid FROM process)
SELECT upid,
       process_name,
       RepeatedField(AndroidCpuMetric_Thread(
                            'name', thread_name,
                            'cpu', thread_proto_cpu)) AS thread_proto
FROM agg_by_thread
GROUP BY upid;

-- Generate Process proto.
CREATE VIEW thread_cpu_view AS
SELECT AndroidCpuMetric_Process(
       'name', process_name,
       'threads', thread_proto) AS cpu_info_process
FROM agg_by_process
GROUP BY upid;

CREATE VIEW android_cpu_output AS
SELECT AndroidCpuMetric(
         'process_info', (SELECT RepeatedField(cpu_info_process) FROM thread_cpu_view));
