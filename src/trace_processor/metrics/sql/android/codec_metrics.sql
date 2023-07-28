--
-- Copyright 2023 The Android Open Source Project
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

SELECT RUN_METRIC('android/android_cpu.sql');

-- Attaching thread proto with media thread name
DROP VIEW IF EXISTS core_type_proto_per_thread_name;
CREATE VIEW core_type_proto_per_thread_name AS
SELECT
thread.name as thread_name,
core_type_proto_per_thread.proto AS proto
FROM core_type_proto_per_thread
JOIN thread using(utid)
WHERE thread.name = 'MediaCodec_loop' OR
      thread.name = 'CodecLooper'
GROUP BY thread.name;

-- aggregate all cpu the codec threads
DROP VIEW IF EXISTS codec_per_thread_cpu_use;
CREATE VIEW codec_per_thread_cpu_use AS
SELECT
  upid,
  process.name AS process_name,
  thread.name AS thread_name,
  CAST(SUM(sched.dur) as INT64) AS cpu_time_ns,
  COUNT(DISTINCT utid) AS num_threads
FROM sched
JOIN thread USING(utid)
JOIN process USING(upid)
WHERE thread.name = 'MediaCodec_loop' OR
      thread.name = 'CodecLooper'
GROUP BY process.name, thread.name;

-- All process that has codec thread
DROP VIEW IF EXISTS android_codec_process;
CREATE VIEW android_codec_process AS
SELECT
  upid,
  process.name as process_name
FROM sched
JOIN thread using(utid)
JOIN process using(upid)
WHERE thread.name = 'MediaCodec_loop' OR
      thread.name = 'CodecLooper'
GROUP BY process_name;

-- Total cpu for a process
DROP VIEW IF EXISTS codec_total_per_process_cpu_use;
CREATE VIEW codec_total_per_process_cpu_use AS
SELECT
  upid,
  process_name,
  CAST(SUM(sched.dur) as INT64) AS media_process_cpu_time_ns
FROM sched
JOIN thread using(utid)
JOIN android_codec_process using(upid)
GROUP BY process_name;

-- Joining total process with media thread table
DROP VIEW IF EXISTS codec_per_process_thread_cpu_use;
CREATE VIEW codec_per_process_thread_cpu_use AS
SELECT
  *
FROM codec_total_per_process_cpu_use
JOIN codec_per_thread_cpu_use using(process_name);

-- Traces are collected using specific traits in codec framework. These traits
-- are mapped to actual names of slices and then combined with other tables to
-- give out the total_cpu and cpu_running time.

-- Utility function to trim codec trace string: extract the string demilited
-- by the limiter.
CREATE PERFETTO FUNCTION extract_codec_string(slice_name STRING, limiter STRING)
RETURNS STRING AS
SELECT CASE
  -- Delimit with the first occurrence
  WHEN instr($slice_name, $limiter) > 0
  THEN substr($slice_name, 1, instr($slice_name, $limiter) - 1)
  ELSE $slice_name
END;

-- traits strings from codec framework
DROP TABLE IF EXISTS trace_trait_table;
CREATE TABLE trace_trait_table(
  trace_trait  varchar(100));
insert into trace_trait_table (trace_trait) values
  ('MediaCodec'),
  ('CCodec'),
  ('C2PooledBlockPool'),
  ('C2BufferQueueBlockPool'),
  ('Codec2'),
  ('ACodec'),
  ('FrameDecoder');

-- Maps traits to slice strings. Any string with '@' is considered to indicate
-- the same trace with different information.Hence those strings are delimited
-- using '@' and considered as part of single trace.
DROP VIEW IF EXISTS codec_slices;
CREATE VIEW codec_slices AS
SELECT
  DISTINCT extract_codec_string(slice.name, '@') as codec_slice_string
FROM slice
JOIN trace_trait_table ON slice.name glob  '*' || trace_trait || '*';

-- combine slice and thread info
DROP VIEW IF EXISTS slice_with_utid;
CREATE VIEW slice_with_utid AS
SELECT
  extract_codec_string(slice.name, '@') as codec_string,
  ts,
  dur,
  upid,
  slice.name as slice_name,
  slice.id as slice_id, utid,
  thread.name as thread_name
FROM slice
JOIN thread_track ON thread_track.id = slice.track_id
JOIN thread USING (utid);

-- Combine with thread_state info
DROP TABLE IF EXISTS slice_thread_state_breakdown;
CREATE VIRTUAL TABLE slice_thread_state_breakdown
USING SPAN_LEFT_JOIN(
  slice_with_utid PARTITIONED utid,
  thread_state PARTITIONED utid
);

-- Get cpu_running_time for all the slices of interest
DROP VIEW IF EXISTS slice_cpu_running;
CREATE VIEW slice_cpu_running AS
SELECT
  codec_string,
  sum(dur) as cpu_time,
  sum(case when state = 'Running' then dur else 0 end) as cpu_run_ns,
  thread_name,
  process.name as process_name,
  slice_id,
  slice_name
FROM slice_thread_state_breakdown
LEFT JOIN process using(upid)
where codec_string in (select codec_slice_string from codec_slices)
GROUP BY codec_string, thread_name, process_name;


-- Generate proto for the trace
DROP VIEW IF EXISTS metrics_per_slice_type;
CREATE VIEW metrics_per_slice_type AS
SELECT
  process_name,
  codec_string,
  AndroidCodecMetrics_Detail(
    'thread_name', thread_name,
    'total_cpu_ns', CAST(cpu_time as INT64),
    'running_cpu_ns', CAST(cpu_run_ns as INT64)
  ) AS proto
FROM slice_cpu_running;

-- Generating codec framework cpu metric
DROP VIEW IF EXISTS codec_metrics_output;
CREATE VIEW codec_metrics_output AS
SELECT AndroidCodecMetrics(
  'cpu_usage', (
    SELECT RepeatedField(
      AndroidCodecMetrics_CpuUsage(
        'process_name', process_name,
        'thread_name', thread_name,
        'thread_cpu_ns', CAST((cpu_time_ns) as INT64),
        'num_threads', num_threads,
        'core_data', core_type_proto_per_thread_name.proto
      )
    ) FROM codec_per_process_thread_cpu_use
      JOIN core_type_proto_per_thread_name using(thread_name)
  ),
  'codec_function', (
    SELECT RepeatedField (
      AndroidCodecMetrics_CodecFunction(
        'codec_string', codec_string,
        'process_name', process_name,
        'detail', metrics_per_slice_type.proto
      )
    ) FROM metrics_per_slice_type
  )
);
