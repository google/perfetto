--
-- Copyright 2022 The Android Open Source Project
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

SELECT IMPORT('android.binder');

-- Count Binder transactions per process
DROP VIEW IF EXISTS binder_metrics_by_process;
CREATE VIEW binder_metrics_by_process AS
SELECT * FROM android_binder_metrics_by_process;

DROP VIEW IF EXISTS android_binder_output;
CREATE VIEW android_binder_output AS
SELECT AndroidBinderMetric(
  'process_breakdown', (
    SELECT RepeatedField(
      AndroidBinderMetric_PerProcessBreakdown(
        'process_name', process_name,
        'pid', pid,
        'slice_name', slice_name,
        'count', event_count
      )
    )
    FROM android_binder_metrics_by_process
  ),
  'unaggregated_txn_breakdown', (
    SELECT RepeatedField(
      AndroidBinderMetric_UnaggregatedTxnBreakdown(
        'aidl_name', aidl_name,
        'binder_txn_id', binder_txn_id,
        'client_process', client_process,
        'client_thread', client_thread,
        'is_main_thread', is_main_thread,
        'client_ts', client_ts,
        'client_dur', client_dur,
        'binder_reply_id', binder_reply_id,
        'server_process', server_process,
        'server_thread', server_thread,
        'server_ts', server_ts,
        'server_dur', server_dur,
        'thread_states', (
          SELECT RepeatedField(
            AndroidBinderMetric_ThreadStateBreakdown(
              'thread_state_type', thread_state_type,
              'thread_state', thread_state,
              'thread_state_dur', thread_state_dur,
              'thread_state_count', thread_state_count
            )
          ) FROM android_sync_binder_thread_state_by_txn t WHERE t.binder_txn_id = android_sync_binder_metrics_by_txn.binder_txn_id
        ),
        'blocked_functions', (
          SELECT RepeatedField(
            AndroidBinderMetric_BlockedFunctionBreakdown(
              'thread_state_type', thread_state_type,
              'blocked_function', blocked_function,
              'blocked_function_dur', blocked_function_dur,
              'blocked_function_count', blocked_function_count
            )
          ) FROM android_sync_binder_blocked_functions_by_txn b WHERE b.binder_txn_id = android_sync_binder_metrics_by_txn.binder_txn_id
        )
      )
    )
    FROM android_sync_binder_metrics_by_txn
  )
);
