-- Copyright 2025 The Android Open Source Project
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

-- Provides unified access to Android Foreground Service events from StatsD.
--
-- Suggested minimal config:
--
-- data_sources: {
--     config: {
--         name: "android.statsd"
--         statsd_config: {
--             atom_id: 60  # ForegroundServiceStateChanged
--         }
--     }
-- }

-- Table for raw Foreground Service state change events from StatsD
CREATE PERFETTO TABLE android_foreground_service_state_changes(
  -- Timestamp of foreground service state change.
  ts TIMESTAMP,
  -- UID of process.
  uid LONG,
  -- Short name of the service.
  short_name STRING,
  -- State of the service ('ENTER' or 'EXIT').
  state STRING,
  -- Whether the service has while-in-use permission.
  allow_while_in_use_permission BOOL,
  -- Reason code for starting the service.
  fgs_start_reason_code LONG,
  -- Target SDK version of the app.
  target_sdk_version LONG,
  -- UID of the calling app.
  calling_uid LONG,
  -- Target SDK version of the calling app.
  caller_target_sdk_version LONG,
  -- Calling UID from temp allow list.
  temp_allow_list_calling_uid LONG,
  -- Whether the notification was deferred.
  fgs_notification_deferred BOOL,
  -- Whether the notification was shown.
  fgs_notification_shown BOOL,
  -- Duration of the foreground service in ms.
  fgs_duration_ms LONG,
  -- Start count of the foreground service.
  fgs_start_count LONG,
  -- Hash of the short name.
  short_name_hash LONG,
  -- Whether the service has notification permission.
  fgs_has_notification_permission BOOL,
  -- Types of the foreground service.
  fgs_types LONG,
  -- Type check code for the foreground service.
  fgs_type_check_code STRING,
  -- Whether the service is a delegate.
  is_delegate BOOL,
  -- Client UID of the delegate.
  delegate_client_uid LONG,
  -- Delegation service.
  delegation_service LONG,
  -- API state.
  api_state STRING,
  -- Duration before FGS start in ms.
  api_before_fgs_start_duration_millis LONG,
  -- Duration after FGS end in ms.
  api_after_fgs_end_duration_millis LONG,
  -- While-in-use reason code with no binding.
  while_in_use_reason_code_no_binding LONG,
  -- While-in-use reason code in bind service.
  while_in_use_reason_code_in_bind_service LONG,
  -- While-in-use reason code by bindings.
  while_in_use_reason_code_by_bindings LONG,
  -- FGS start reason code with no binding.
  fgs_start_reason_code_no_binding LONG,
  -- FGS start reason code in bind service.
  fgs_start_reason_code_in_bind_service LONG,
  -- FGS start reason code by bindings.
  fgs_start_reason_code_by_bindings LONG,
  -- FGS start API.
  fgs_start_api STRING,
  -- Whether FGS restriction was recalculated.
  fgs_restriction_recalculated BOOL
)
AS
SELECT
  s.ts,
  extract_arg(s.arg_set_id, 'foreground_service_state_changed.uid') AS uid,
  extract_arg(s.arg_set_id, 'foreground_service_state_changed.short_name') AS short_name,
  -- 'ENTER' or 'EXIT'
  extract_arg(s.arg_set_id, 'foreground_service_state_changed.state') AS state,
  extract_arg(
    s.arg_set_id,
    'foreground_service_state_changed.allow_while_in_use_permission'
  ) AS allow_while_in_use_permission,
  extract_arg(
    s.arg_set_id,
    'foreground_service_state_changed.fgs_start_reason_code'
  ) AS fgs_start_reason_code,
  extract_arg(
    s.arg_set_id,
    'foreground_service_state_changed.target_sdk_version'
  ) AS target_sdk_version,
  extract_arg(s.arg_set_id, 'foreground_service_state_changed.calling_uid') AS calling_uid,
  extract_arg(
    s.arg_set_id,
    'foreground_service_state_changed.caller_target_sdk_version'
  ) AS caller_target_sdk_version,
  extract_arg(
    s.arg_set_id,
    'foreground_service_state_changed.temp_allow_list_calling_uid'
  ) AS temp_allow_list_calling_uid,
  extract_arg(
    s.arg_set_id,
    'foreground_service_state_changed.fgs_notification_deferred'
  ) AS fgs_notification_deferred,
  extract_arg(
    s.arg_set_id,
    'foreground_service_state_changed.fgs_notification_shown'
  ) AS fgs_notification_shown,
  extract_arg(s.arg_set_id, 'foreground_service_state_changed.fgs_duration_ms') AS fgs_duration_ms,
  extract_arg(s.arg_set_id, 'foreground_service_state_changed.fgs_start_count') AS fgs_start_count,
  extract_arg(s.arg_set_id, 'foreground_service_state_changed.short_name_hash') AS short_name_hash,
  extract_arg(
    s.arg_set_id,
    'foreground_service_state_changed.fgs_has_notification_permission'
  ) AS fgs_has_notification_permission,
  extract_arg(s.arg_set_id, 'foreground_service_state_changed.fgs_types') AS fgs_types,
  extract_arg(
    s.arg_set_id,
    'foreground_service_state_changed.fgs_type_check_code'
  ) AS fgs_type_check_code,
  extract_arg(s.arg_set_id, 'foreground_service_state_changed.is_delegate') AS is_delegate,
  extract_arg(
    s.arg_set_id,
    'foreground_service_state_changed.delegate_client_uid'
  ) AS delegate_client_uid,
  extract_arg(
    s.arg_set_id,
    'foreground_service_state_changed.delegation_service'
  ) AS delegation_service,
  extract_arg(s.arg_set_id, 'foreground_service_state_changed.api_state') AS api_state,
  extract_arg(
    s.arg_set_id,
    'foreground_service_state_changed.api_before_fgs_start_duration_millis'
  ) AS api_before_fgs_start_duration_millis,
  extract_arg(
    s.arg_set_id,
    'foreground_service_state_changed.api_after_fgs_end_duration_millis'
  ) AS api_after_fgs_end_duration_millis,
  extract_arg(
    s.arg_set_id,
    'foreground_service_state_changed.while_in_use_reason_code_no_binding'
  ) AS while_in_use_reason_code_no_binding,
  extract_arg(
    s.arg_set_id,
    'foreground_service_state_changed.while_in_use_reason_code_in_bind_service'
  ) AS while_in_use_reason_code_in_bind_service,
  extract_arg(
    s.arg_set_id,
    'foreground_service_state_changed.while_in_use_reason_code_by_bindings'
  ) AS while_in_use_reason_code_by_bindings,
  extract_arg(
    s.arg_set_id,
    'foreground_service_state_changed.fgs_start_reason_code_no_binding'
  ) AS fgs_start_reason_code_no_binding,
  extract_arg(
    s.arg_set_id,
    'foreground_service_state_changed.fgs_start_reason_code_in_bind_service'
  ) AS fgs_start_reason_code_in_bind_service,
  extract_arg(
    s.arg_set_id,
    'foreground_service_state_changed.fgs_start_reason_code_by_bindings'
  ) AS fgs_start_reason_code_by_bindings,
  extract_arg(s.arg_set_id, 'foreground_service_state_changed.fgs_start_api') AS fgs_start_api,
  extract_arg(
    s.arg_set_id,
    'foreground_service_state_changed.fgs_restriction_recalculated'
  ) AS fgs_restriction_recalculated
FROM slice AS s
JOIN track AS t
  ON s.track_id = t.id
WHERE
  t.name = 'Statsd Atoms'
  AND s.name = 'foreground_service_state_changed';

-- View to get foreground service state intervals
CREATE PERFETTO VIEW android_foreground_service_state(
  -- Timestamp of foreground service state change.
  ts TIMESTAMP,
  -- Duration of foreground service state.
  dur DURATION,
  -- UID of process.
  uid LONG,
  -- Short name of the service.
  short_name STRING,
  -- State of the service ('ENTER' or 'EXIT').
  state STRING,
  -- Whether the service has while-in-use permission.
  allow_while_in_use_permission BOOL,
  -- Reason code for starting the service.
  fgs_start_reason_code LONG,
  -- Target SDK version of the app.
  target_sdk_version LONG,
  -- UID of the calling app.
  calling_uid LONG,
  -- Target SDK version of the calling app.
  caller_target_sdk_version LONG,
  -- Calling UID from temp allow list.
  temp_allow_list_calling_uid LONG,
  -- Whether the notification was deferred.
  fgs_notification_deferred BOOL,
  -- Whether the notification was shown.
  fgs_notification_shown BOOL,
  -- Duration of the foreground service in ms.
  fgs_duration_ms LONG,
  -- Start count of the foreground service.
  fgs_start_count LONG,
  -- Hash of the short name.
  short_name_hash LONG,
  -- Whether the service has notification permission.
  fgs_has_notification_permission BOOL,
  -- Types of the foreground service.
  fgs_types LONG,
  -- Type check code for the foreground service.
  fgs_type_check_code STRING,
  -- Whether the service is a delegate.
  is_delegate BOOL,
  -- Client UID of the delegate.
  delegate_client_uid LONG,
  -- Delegation service.
  delegation_service LONG,
  -- API state.
  api_state STRING,
  -- Duration before FGS start in ms.
  api_before_fgs_start_duration_millis LONG,
  -- Duration after FGS end in ms.
  api_after_fgs_end_duration_millis LONG,
  -- While-in-use reason code with no binding.
  while_in_use_reason_code_no_binding LONG,
  -- While-in-use reason code in bind service.
  while_in_use_reason_code_in_bind_service LONG,
  -- While-in-use reason code by bindings.
  while_in_use_reason_code_by_bindings LONG,
  -- FGS start reason code with no binding.
  fgs_start_reason_code_no_binding LONG,
  -- FGS start reason code in bind service.
  fgs_start_reason_code_in_bind_service LONG,
  -- FGS start reason code by bindings.
  fgs_start_reason_code_by_bindings LONG,
  -- FGS start API.
  fgs_start_api STRING,
  -- Whether FGS restriction was recalculated.
  fgs_restriction_recalculated BOOL
)
AS
SELECT
  ts,
  lead(ts, 1, (SELECT end_ts FROM trace_bounds)) OVER (
    PARTITION BY
      uid,
      short_name
    ORDER BY ts
  )
  - ts AS dur,
  uid,
  short_name,
  state,
  -- Keep all other fields from the base table
  allow_while_in_use_permission,
  fgs_start_reason_code,
  target_sdk_version,
  calling_uid,
  caller_target_sdk_version,
  temp_allow_list_calling_uid,
  fgs_notification_deferred,
  fgs_notification_shown,
  fgs_duration_ms,
  fgs_start_count,
  short_name_hash,
  fgs_has_notification_permission,
  fgs_types,
  fgs_type_check_code,
  is_delegate,
  delegate_client_uid,
  delegation_service,
  api_state,
  api_before_fgs_start_duration_millis,
  api_after_fgs_end_duration_millis,
  while_in_use_reason_code_no_binding,
  while_in_use_reason_code_in_bind_service,
  while_in_use_reason_code_by_bindings,
  fgs_start_reason_code_no_binding,
  fgs_start_reason_code_in_bind_service,
  fgs_start_reason_code_by_bindings,
  fgs_start_api,
  fgs_restriction_recalculated
FROM android_foreground_service_state_changes
WHERE
  state = 'ENTER';
