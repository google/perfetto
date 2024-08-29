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

INCLUDE PERFETTO MODULE android.process_metadata;

-- Establish relationships between thread and process
CREATE PERFETTO TABLE _thread_process_summary AS
SELECT
  thread.utid,
  thread.upid,
  thread.tid,
  process.pid,
  thread.name as thread_name,
  process.name as process_name
FROM thread
LEFT JOIN process USING (upid);

-- Add thread_state info to thread/process/package
CREATE PERFETTO TABLE _state_w_thread_process_summary AS
SELECT
  thread_state.ts,
  thread_state.dur,
  thread_state.cpu,
  thread_state.state,
  m.utid,
  m.upid,
  m.tid,
  m.pid,
  m.thread_name,
  m.process_name
FROM _thread_process_summary as m
JOIN thread_state USING (utid);

-- Add scheduling slices info to thread/process/package
CREATE PERFETTO TABLE _sched_w_thread_process_package_summary AS
SELECT
  sched.ts,
  sched.dur,
  sched.cpu,
  m.utid,
  m.upid,
  m.tid,
  m.pid,
  package.uid,
  m.thread_name,
  m.process_name,
  package.package_name
FROM _thread_process_summary as m
JOIN sched USING (utid)
LEFT JOIN android_process_metadata as package USING(upid)
WHERE dur > 0;
