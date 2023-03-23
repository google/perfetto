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

SELECT IMPORT('android.monitor_contention');

DROP VIEW IF EXISTS android_monitor_contention_output;
CREATE VIEW android_monitor_contention_output AS
SELECT AndroidMonitorContentionMetric(
  'node', (
    SELECT RepeatedField(
      AndroidMonitorContentionMetric_Node(
        'node_parent_id', parent_id,
        'node_id', id,
        'ts', ts,
        'dur', dur,
        'blocking_method', blocking_method,
        'blocked_method', blocked_method,
        'short_blocking_method', short_blocking_method,
        'short_blocked_method', short_blocked_method,
        'blocking_src', blocking_src,
        'blocked_src', blocked_src,
        'waiter_count', waiter_count,
        'blocking_thread_name', blocking_thread_name,
        'blocked_thread_name', blocked_thread_name,
        'process_name', process_name,
        'is_blocked_thread_main', is_blocked_thread_main,
        'is_blocking_thread_main', is_blocking_thread_main,
        'binder_reply_ts', binder_reply_ts,
        'binder_reply_tid', binder_reply_tid
      )
    )
    FROM android_monitor_contention_chain
  )
);
