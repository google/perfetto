--
-- Copyright 2026 The Android Open Source Project
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

-- Checks if a specific job state flag is set in the bitmask.
CREATE PERFETTO FUNCTION _android_js_has_job_state_flag(
    -- Bitmask of job state flags.
    flags LONG,
    -- Name of the flag to check.
    flag_name STRING
)
RETURNS BOOL AS
SELECT
  CASE $flag_name
    -- App requested job should run when device is charging.
    WHEN 'HAS_CHARGING_CONSTRAINT'
    THEN (
      $flags & (
        1 << 0
      )
    ) != 0
    -- App requested job should run when battery is not low.
    WHEN 'HAS_BATTERY_NOT_LOW_CONSTRAINT'
    THEN (
      $flags & (
        1 << 1
      )
    ) != 0
    -- App requested job should run when storage is not low.
    WHEN 'HAS_STORAGE_NOT_LOW_CONSTRAINT'
    THEN (
      $flags & (
        1 << 2
      )
    ) != 0
    -- App requested job has a timing delay.
    WHEN 'HAS_TIMING_DELAY_CONSTRAINT'
    THEN (
      $flags & (
        1 << 3
      )
    ) != 0
    -- App requested a deadline by which the job should run.
    WHEN 'HAS_DEADLINE_CONSTRAINT'
    THEN (
      $flags & (
        1 << 4
      )
    ) != 0
    -- App requested job should run when device is idle.
    WHEN 'HAS_IDLE_CONSTRAINT'
    THEN (
      $flags & (
        1 << 5
      )
    ) != 0
    -- App requested job should run when device has connectivity.
    WHEN 'HAS_CONNECTIVITY_CONSTRAINT'
    THEN (
      $flags & (
        1 << 6
      )
    ) != 0
    -- App requested job should run when there is a content trigger.
    WHEN 'HAS_CONTENT_TRIGGER_CONSTRAINT'
    THEN (
      $flags & (
        1 << 7
      )
    ) != 0
    -- App requested this run as an expedited job.
    WHEN 'IS_REQUESTED_EXPEDITED_JOB'
    THEN (
      $flags & (
        1 << 8
      )
    ) != 0
    -- Job is currently running as an expedited job.
    WHEN 'IS_RUNNING_AS_EXPEDITED_JOB'
    THEN (
      $flags & (
        1 << 9
      )
    ) != 0
    -- App requested job should run within hours of app launch.
    WHEN 'IS_PREFETCH'
    THEN (
      $flags & (
        1 << 10
      )
    ) != 0
    -- App requested this run as a user-initiated job.
    WHEN 'IS_REQUESTED_AS_USER_INITIATED_JOB'
    THEN (
      $flags & (
        1 << 19
      )
    ) != 0
    -- Job is currently running as a user-initiated job.
    WHEN 'IS_RUNNING_AS_USER_INITIATED_JOB'
    THEN (
      $flags & (
        1 << 20
      )
    ) != 0
    -- Job is configured to run in intervals.
    WHEN 'IS_PERIODIC'
    THEN (
      $flags & (
        1 << 21
      )
    ) != 0
    -- Job should run as a flex job.
    WHEN 'HAS_FLEXIBILITY_CONSTRAINT'
    THEN (
      $flags & (
        1 << 22
      )
    ) != 0
    -- Transport preference logic can be applied with flex policy.
    WHEN 'CAN_APPLY_TRANSPORT_AFFINITIES'
    THEN (
      $flags & (
        1 << 24
      )
    ) != 0
    ELSE FALSE
  END;

-- Converts job state integer to string name.
CREATE PERFETTO FUNCTION _android_js_get_job_state_name(
    -- Job state integer.
    state LONG
)
RETURNS STRING AS
SELECT
  CASE $state
    -- Job started executing and has finished.
    WHEN 0
    THEN 'FINISHED'
    -- Scheduled job has just started executing.
    WHEN 1
    THEN 'STARTED'
    -- App just scheduled this job to be executed in the future.
    WHEN 2
    THEN 'SCHEDULED'
    -- Job was scheduled but cancelled before it started executing.
    WHEN 3
    THEN 'CANCELLED'
    ELSE 'UNKNOWN'
  END;

-- Converts standby bucket integer to string name.
-- Standby buckets are used to prioritize background work.
CREATE PERFETTO FUNCTION _android_js_get_standby_bucket_name(
    -- Standby bucket integer.
    bucket LONG
)
RETURNS STRING AS
SELECT
  CASE $bucket
    -- App is being used or was used very recently.
    WHEN 0
    THEN 'ACTIVE'
    -- App is being used regularly.
    WHEN 1
    THEN 'WORKING_SET'
    -- App is used occasionally.
    WHEN 2
    THEN 'FREQUENT'
    -- App is used rarely.
    WHEN 3
    THEN 'RARE'
    -- App has not been used for several days.
    WHEN 4
    THEN 'NEVER'
    -- App is restricted (e.g. by user or system).
    WHEN 5
    THEN 'RESTRICTED'
    -- App is exempt from standby restrictions.
    WHEN 6
    THEN 'EXEMPTED'
    ELSE 'UNKNOWN'
  END;

-- Converts internal stop reason integer to string name.
CREATE PERFETTO FUNCTION _android_js_get_internal_stop_reason_name(
    -- Internal stop reason integer.
    reason LONG
)
RETURNS STRING AS
SELECT
  CASE $reason
    WHEN -1
    THEN 'INTERNAL_STOP_REASON_UNKNOWN'
    -- App or system cancelled the job.
    WHEN 0
    THEN 'INTERNAL_STOP_REASON_CANCELED'
    -- One or more constraints (e.g. charging, network) were lost.
    WHEN 1
    THEN 'INTERNAL_STOP_REASON_CONSTRAINTS_NOT_SATISFIED'
    -- Job was preempted by a higher priority task.
    WHEN 2
    THEN 'INTERNAL_STOP_REASON_PREEMPT'
    -- Job execution exceeded the system timeout.
    WHEN 3
    THEN 'INTERNAL_STOP_REASON_TIMEOUT'
    -- Device entered idle state, causing job to stop.
    WHEN 4
    THEN 'INTERNAL_STOP_REASON_DEVICE_IDLE'
    -- Device thermal limits reached.
    WHEN 5
    THEN 'INTERNAL_STOP_REASON_DEVICE_THERMAL'
    -- App moved to restricted standby bucket.
    WHEN 6
    THEN 'INTERNAL_STOP_REASON_RESTRICTED_BUCKET'
    -- Package was uninstalled.
    WHEN 7
    THEN 'INTERNAL_STOP_REASON_UNINSTALL'
    -- App data was cleared.
    WHEN 8
    THEN 'INTERNAL_STOP_REASON_DATA_CLEARED'
    -- System time changed.
    WHEN 9
    THEN 'INTERNAL_STOP_REASON_RTC_UPDATED'
    -- Job completed successfully.
    WHEN 10
    THEN 'INTERNAL_STOP_REASON_SUCCESSFUL_FINISH'
    -- User manually stopped the job via UI.
    WHEN 11
    THEN 'INTERNAL_STOP_REASON_USER_UI_STOP'
    -- App Not Responding.
    WHEN 12
    THEN 'INTERNAL_STOP_REASON_ANR'
    -- JobService.jobFinished() never called on the job it
    -- consecutively timed out.
    WHEN 13
    THEN 'INTERNAL_STOP_REASON_TIMEOUT_ABANDONED'
    ELSE 'INTERNAL_STOP_REASON_UNKNOWN'
  END;

-- Converts public stop reason integer to string name.
CREATE PERFETTO FUNCTION _android_js_get_public_stop_reason_name(
    -- Public stop reason integer.
    reason LONG
)
RETURNS STRING AS
SELECT
  CASE $reason
    WHEN 0
    THEN 'STOP_REASON_UNDEFINED'
    -- JobService.cancel() was called.
    WHEN 1
    THEN 'STOP_REASON_CANCELLED_BY_APP'
    -- System preempted the job.
    WHEN 2
    THEN 'STOP_REASON_PREEMPT'
    -- Execution timed out.
    WHEN 3
    THEN 'STOP_REASON_TIMEOUT'
    -- Device state change (e.g. battery, screen).
    WHEN 4
    THEN 'STOP_REASON_DEVICE_STATE'
    -- Battery low constraint lost.
    WHEN 5
    THEN 'STOP_REASON_CONSTRAINT_BATTERY_NOT_LOW'
    -- Charging constraint lost.
    WHEN 6
    THEN 'STOP_REASON_CONSTRAINT_CHARGING'
    -- Connectivity constraint lost.
    WHEN 7
    THEN 'STOP_REASON_CONSTRAINT_CONNECTIVITY'
    -- Idle constraint lost.
    WHEN 8
    THEN 'STOP_REASON_CONSTRAINT_DEVICE_IDLE'
    -- Storage low constraint lost.
    WHEN 9
    THEN 'STOP_REASON_CONSTRAINT_STORAGE_NOT_LOW'
    -- App exceeded its job execution quota.
    WHEN 10
    THEN 'STOP_REASON_QUOTA'
    -- App background execution restricted.
    WHEN 11
    THEN 'STOP_REASON_BACKGROUND_RESTRICTION'
    -- Standby bucket restriction.
    WHEN 12
    THEN 'STOP_REASON_APP_STANDBY'
    -- User-initiated stop.
    WHEN 13
    THEN 'STOP_REASON_USER'
    -- System needs to reclaim resources.
    WHEN 14
    THEN 'STOP_REASON_SYSTEM_PROCESSING'
    -- Estimated app launch time changed significantly.
    WHEN 15
    THEN 'STOP_REASON_ESTIMATED_APP_LAUNCH_TIME_CHANGED'
    -- JobService.jobFinished() never called on the job it
    -- consecutively timed out.
    WHEN 16
    THEN 'STOP_REASON_TIMEOUT_ABANDONED'
    ELSE 'STOP_REASON_UNDEFINED'
  END;

-- Converts backoff policy integer to string name.
CREATE PERFETTO FUNCTION _android_js_get_backoff_policy(
    -- Backoff policy integer.
    policy_type LONG
)
RETURNS STRING AS
SELECT
  CASE $policy_type
    WHEN 0
    THEN 'UNKNOWN_POLICY'
    -- The backoff time is increased linearly (e.g. 10s, 20s, 30s...).
    WHEN 1
    THEN 'LINEAR'
    -- The backoff time is increased exponentially (e.g. 10s, 20s, 40s...).
    WHEN 2
    THEN 'EXPONENTIAL'
    ELSE 'UNKNOWN_POLICY'
  END;

-- Converts proc state integer to string name.
-- Process states are defined in ActivityManager.
CREATE PERFETTO FUNCTION _android_js_get_proc_state_name(
    -- Process state integer.
    state LONG
)
RETURNS STRING AS
SELECT
  CASE $state
    WHEN 0
    THEN 'PROCESS_STATE_UNSPECIFIED'
    WHEN 998
    THEN 'PROCESS_STATE_UNKNOWN_TO_PROTO'
    WHEN 999
    THEN 'PROCESS_STATE_UNKNOWN'
    -- Persistent system process.
    WHEN 1000
    THEN 'PROCESS_STATE_PERSISTENT'
    WHEN 1001
    THEN 'PROCESS_STATE_PERSISTENT_UI'
    -- Process is in the foreground and interacting with the user.
    WHEN 1002
    THEN 'PROCESS_STATE_TOP'
    -- Process is running a foreground service.
    WHEN 1003
    THEN 'PROCESS_STATE_FOREGROUND_SERVICE'
    WHEN 1004
    THEN 'PROCESS_STATE_BOUND_FOREGROUND_SERVICE'
    WHEN 1005
    THEN 'PROCESS_STATE_IMPORTANT_FOREGROUND'
    WHEN 1006
    THEN 'PROCESS_STATE_IMPORTANT_BACKGROUND'
    WHEN 1007
    THEN 'PROCESS_STATE_TRANSIENT_BACKGROUND'
    WHEN 1008
    THEN 'PROCESS_STATE_BACKUP'
    WHEN 1009
    THEN 'PROCESS_STATE_SERVICE'
    WHEN 1010
    THEN 'PROCESS_STATE_RECEIVER'
    WHEN 1011
    THEN 'PROCESS_STATE_TOP_SLEEPING'
    WHEN 1012
    THEN 'PROCESS_STATE_HEAVY_WEIGHT'
    WHEN 1013
    THEN 'PROCESS_STATE_HOME'
    WHEN 1014
    THEN 'PROCESS_STATE_LAST_ACTIVITY'
    WHEN 1015
    THEN 'PROCESS_STATE_CACHED_ACTIVITY'
    WHEN 1016
    THEN 'PROCESS_STATE_CACHED_ACTIVITY_CLIENT'
    WHEN 1017
    THEN 'PROCESS_STATE_CACHED_RECENT'
    -- Process is being kept in memory for potential future use.
    WHEN 1018
    THEN 'PROCESS_STATE_CACHED_EMPTY'
    WHEN 1019
    THEN 'PROCESS_STATE_NONEXISTENT'
    WHEN 1020
    THEN 'PROCESS_STATE_BOUND_TOP'
    ELSE 'PROCESS_STATE_UNKNOWN'
  END;

-- Extracts trace tag from job name if job name is in format #tag#...
CREATE PERFETTO FUNCTION _android_js_extract_trace_tag(
    job_name STRING
)
RETURNS STRING AS
SELECT
  coalesce(regexp_extract($job_name, '#(.*)#'), '');

-- Converts priority integer to string name.
CREATE PERFETTO FUNCTION _android_js_get_priority_name(
    priority LONG
)
RETURNS STRING AS
SELECT
  CASE
    WHEN $priority = 100
    THEN 'PRIORITY_MIN'
    WHEN $priority = 200
    THEN 'PRIORITY_LOW'
    WHEN $priority = 300
    THEN 'PRIORITY_DEFAULT'
    WHEN $priority = 400
    THEN 'PRIORITY_HIGH'
    WHEN $priority = 500
    THEN 'PRIORITY_MAX'
    ELSE 'PRIORITY_UNKNOWN'
  END;

CREATE PERFETTO FUNCTION _android_js_extract_package_name(
    job_name STRING
)
RETURNS STRING AS
SELECT
  coalesce(
    regexp_extract($job_name, '([a-zA-Z0-9_.-]+)/'),
    regexp_extract($job_name, ':([a-zA-Z0-9_.-]*)$')
  );

-- Extracts job scheduler arg from arg_set_id.
CREATE PERFETTO FUNCTION _android_js_extract_js_arg(
    arg_set_id LONG,
    field_name STRING
)
RETURNS STRING AS
SELECT
  coalesce(
    extract_arg($arg_set_id, 'job_scheduler_job.' || $field_name),
    extract_arg($arg_set_id, 'debug.job_scheduler_job.' || $field_name),
    extract_arg($arg_set_id, 'debug.job_scheduler_job.' || $field_name),
    extract_arg($arg_set_id, 'debug.job_scheduler_job_' || $field_name)
  );

-- Extracts namespace from job name if job name is in format @namespace@...
CREATE PERFETTO FUNCTION _android_js_extract_job_namespace(
    job_name STRING
)
RETURNS STRING AS
SELECT
  coalesce(regexp_extract($job_name, '@(.*)@'), '');

-- Create a table containing all JobScheduler job state changes from slices.
CREATE PERFETTO TABLE _android_js_job_state_events AS
WITH
  raw_job_states AS (
    SELECT
      s.ts,
      s.id AS slice_id,
      s.name AS job_name,
      s.arg_set_id,
      CAST(_android_js_extract_js_arg(s.arg_set_id, 'job_state_flags') AS INTEGER) AS job_state_flags
    FROM track AS t
    JOIN slice AS s
      ON (
        s.track_id = t.id
      )
    WHERE
      s.category = 'jobscheduler'
  )
SELECT
  ts,
  job_name,
  slice_id,
  CAST(_android_js_extract_js_arg(arg_set_id, 'job_id') AS INTEGER) AS job_id,
  CAST(_android_js_extract_js_arg(arg_set_id, 'source_uid') AS INTEGER) AS uid,
  CAST(_android_js_extract_js_arg(arg_set_id, 'proxy_uid') AS INTEGER) AS proxy_uid,
  _android_js_get_job_state_name(CAST(_android_js_extract_js_arg(arg_set_id, 'state') AS INTEGER)) AS state,
  _android_js_get_standby_bucket_name(CAST(_android_js_extract_js_arg(arg_set_id, 'standby_bucket') AS INTEGER)) AS standby_bucket,
  _android_js_get_priority_name(CAST(_android_js_extract_js_arg(arg_set_id, 'requested_priority') AS INTEGER)) AS requested_priority,
  _android_js_get_priority_name(CAST(_android_js_extract_js_arg(arg_set_id, 'effective_priority') AS INTEGER)) AS effective_priority,
  CAST(_android_js_extract_js_arg(arg_set_id, 'num_previous_attempts') AS INTEGER) AS num_previous_attempts,
  CAST(_android_js_extract_js_arg(arg_set_id, 'deadline_ms') AS INTEGER) AS deadline_ms,
  CAST(_android_js_extract_js_arg(arg_set_id, 'delay_ms') AS INTEGER) AS delay_ms,
  CAST(_android_js_extract_js_arg(arg_set_id, 'job_start_latency_ms') AS INTEGER) AS job_start_latency_ms,
  CAST(_android_js_extract_js_arg(arg_set_id, 'num_uncompleted_work_items') AS INTEGER) AS num_uncompleted_work_items,
  _android_js_get_proc_state_name(CAST(_android_js_extract_js_arg(arg_set_id, 'proc_state') AS INTEGER)) AS proc_state,
  _android_js_get_internal_stop_reason_name(CAST(_android_js_extract_js_arg(arg_set_id, 'internal_stop_reason') AS INTEGER)) AS internal_stop_reason,
  _android_js_get_public_stop_reason_name(CAST(_android_js_extract_js_arg(arg_set_id, 'public_stop_reason') AS INTEGER)) AS public_stop_reason,
  CAST(_android_js_extract_js_arg(arg_set_id, 'periodic_job_interval_ms') AS INTEGER) AS periodic_job_interval_ms,
  CAST(_android_js_extract_js_arg(arg_set_id, 'periodic_job_flex_interval_ms') AS INTEGER) AS periodic_job_flex_interval_ms,
  _android_js_extract_trace_tag(job_name) AS filtered_trace_tag,
  CAST(_android_js_extract_js_arg(arg_set_id, 'num_reschedules_due_to_abandonment') AS INTEGER) AS num_reschedules_due_to_abandonment,
  _android_js_get_backoff_policy(CAST(_android_js_extract_js_arg(arg_set_id, 'back_off_policy_type') AS INTEGER)) AS back_off_policy_type,
  _android_js_has_job_state_flag(job_state_flags, 'HAS_CHARGING_CONSTRAINT') AS has_charging_constraint,
  _android_js_has_job_state_flag(job_state_flags, 'HAS_BATTERY_NOT_LOW_CONSTRAINT') AS has_battery_not_low_constraint,
  _android_js_has_job_state_flag(job_state_flags, 'HAS_STORAGE_NOT_LOW_CONSTRAINT') AS has_storage_not_low_constraint,
  _android_js_has_job_state_flag(job_state_flags, 'HAS_TIMING_DELAY_CONSTRAINT') AS has_timing_delay_constraint,
  _android_js_has_job_state_flag(job_state_flags, 'HAS_DEADLINE_CONSTRAINT') AS has_deadline_constraint,
  _android_js_has_job_state_flag(job_state_flags, 'HAS_IDLE_CONSTRAINT') AS has_idle_constraint,
  _android_js_has_job_state_flag(job_state_flags, 'HAS_CONNECTIVITY_CONSTRAINT') AS has_connectivity_constraint,
  _android_js_has_job_state_flag(job_state_flags, 'HAS_CONTENT_TRIGGER_CONSTRAINT') AS has_content_trigger_constraint,
  _android_js_has_job_state_flag(job_state_flags, 'IS_REQUESTED_EXPEDITED_JOB') AS is_requested_expedited_job,
  _android_js_has_job_state_flag(job_state_flags, 'IS_RUNNING_AS_EXPEDITED_JOB') AS is_running_as_expedited_job,
  _android_js_has_job_state_flag(job_state_flags, 'IS_PREFETCH') AS is_prefetch,
  _android_js_has_job_state_flag(job_state_flags, 'IS_REQUESTED_AS_USER_INITIATED_JOB') AS is_requested_as_user_initiated_job,
  _android_js_has_job_state_flag(job_state_flags, 'IS_RUNNING_AS_USER_INITIATED_JOB') AS is_running_as_user_initiated_job,
  _android_js_has_job_state_flag(job_state_flags, 'IS_PERIODIC') AS is_periodic,
  _android_js_has_job_state_flag(job_state_flags, 'HAS_FLEXIBILITY_CONSTRAINT') AS has_flexibility_constraint,
  _android_js_has_job_state_flag(job_state_flags, 'CAN_APPLY_TRANSPORT_AFFINITIES') AS can_apply_transport_affinities
FROM raw_job_states;

-- Create table with job execution intervals (state='STARTED').
CREATE PERFETTO TABLE _android_js_job_started AS
WITH
  job_states_with_lead AS (
    SELECT
      *,
      lead(state, 1) OVER job_asc AS lead_state,
      lead(ts, 1, trace_end()) OVER job_asc AS ts_lead,
      lead(ts, 1) OVER job_asc IS NULL AS is_end_slice,
      lead(internal_stop_reason, 1, 'INTERNAL_STOP_REASON_UNKNOWN') OVER job_asc AS lead_internal_stop_reason,
      lead(public_stop_reason, 1, 'PUBLIC_STOP_REASON_UNKNOWN') OVER job_asc AS lead_public_stop_reason,
      lag(requested_priority, 1, 'PRIORITY_UNKNOWN') OVER job_asc AS lag_requested_priority
    FROM _android_js_job_state_events
    WHERE
      state != 'CANCELLED'
    WINDOW job_asc AS (PARTITION BY uid, job_name, job_id ORDER BY ts)
  )
SELECT
  _android_js_extract_package_name(job_name) AS package_name,
  _android_js_extract_job_namespace(job_name) AS job_namespace,
  ts_lead - ts AS dur,
  iif(lead_state = 'SCHEDULED', TRUE, FALSE) AS is_rescheduled,
  coalesce(nullif(requested_priority, 'PRIORITY_UNKNOWN'), lag_requested_priority) AS effective_requested_priority,
  *
FROM job_states_with_lead
WHERE
  is_end_slice = FALSE
  AND (
    ts_lead - ts
  ) > 0
  AND state = 'STARTED'
  AND lead_state IN ('FINISHED', 'SCHEDULED');

-- This table returns all running jobs from job scheduler track events.
--
-- Values in this table are derived from job scheduler track events.
-- To populate this table, the `jobscheduler` category must be enabled
-- in the `track_event` data source.
CREATE PERFETTO TABLE android_job_scheduler_states_track_events (
  -- Unique identifier for job scheduler state.
  id LONG,
  -- Id of the slice.
  slice_id JOINID(slice.id),
  -- Timestamp of job state slice.
  ts TIMESTAMP,
  -- Duration of job state slice.
  dur DURATION,
  -- Name of the job (as named by the app).
  job_name STRING,
  -- Package that the job belongs (ex: associated app).
  package_name STRING,
  -- Namespace of job.
  job_namespace STRING,
  -- Id of job.
  job_id LONG,
  -- Uid associated with the job.
  uid LONG,
  -- Uid associated with proxy job.
  proxy_uid LONG,
  -- Trace tag set via JobInfo.Builder.setTraceTag().
  filtered_trace_tag STRING,
  -- The job's standby bucket.
  standby_bucket STRING,
  -- The requested priority at which the job should run.
  requested_priority STRING,
  -- The effective priority at which the job ran.
  effective_priority STRING,
  -- Number of previous attempts at running job.
  num_previous_attempts LONG,
  -- Deadline that job has requested.
  deadline_ms LONG,
  -- The delay that the Job has requested.
  delay_ms LONG,
  -- The latency in ms between scheduling and starting.
  job_start_latency_ms LONG,
  -- Number of uncompleted job work items.
  num_uncompleted_work_items LONG,
  -- Process state of the process responsible for running the job.
  proc_state STRING,
  -- Interval for the job to recur when it is set as periodic.
  periodic_job_interval_ms LONG,
  -- Flex interval for the periodic job.
  periodic_job_flex_interval_ms LONG,
  -- Number of reschedules due to job being abandoned.
  num_reschedules_due_to_abandonment LONG,
  -- Back off policy applied to the job that gets rescheduled.
  back_off_policy_type STRING,
  -- Internal stop reason for a job.
  internal_stop_reason STRING,
  -- Public stop reason for a job.
  public_stop_reason STRING,
  -- True if app requested job should run when the device is charging.
  has_charging_constraint BOOL,
  -- True if app requested job should run when the device battery is not low.
  has_battery_not_low_constraint BOOL,
  -- True if app requested job should run when device storage is not low.
  has_storage_not_low_constraint BOOL,
  -- True if app requested job has a timing delay.
  has_timing_delay_constraint BOOL,
  -- True if app requested there is a deadline by which the job should run.
  has_deadline_constraint BOOL,
  -- True if app requested job should run when device is idle.
  has_idle_constraint BOOL,
  -- True if app requested job should run when device has connectivity.
  has_connectivity_constraint BOOL,
  -- True if app requested job should run when there is a content trigger.
  has_content_trigger_constraint BOOL,
  -- True if app requested that the job is run as an expedited job.
  is_requested_expedited_job BOOL,
  -- The job is run as an expedited job.
  is_running_as_expedited_job BOOL,
  -- True if app requested job should run within hours of app launch.
  is_prefetch BOOL,
  -- True is app has requested that a job be run as a user initiated job.
  is_requested_as_user_initiated_job BOOL,
  -- True if job is running as a user initiated job.
  is_running_as_user_initiated_job BOOL,
  -- Job should run in intervals.
  is_periodic BOOL,
  -- True if the job should run as a flex job.
  has_flexibility_constraint BOOL,
  -- Whether transport preference logic can be applied to this job.
  can_apply_transport_affinities BOOL
) AS
SELECT
  row_number() OVER (ORDER BY ts) AS id,
  slice_id,
  ts,
  dur,
  job_name,
  package_name,
  job_namespace,
  job_id,
  uid,
  proxy_uid,
  filtered_trace_tag,
  standby_bucket,
  effective_requested_priority AS requested_priority,
  effective_priority,
  num_previous_attempts,
  deadline_ms,
  delay_ms,
  job_start_latency_ms,
  num_uncompleted_work_items,
  proc_state,
  periodic_job_interval_ms,
  periodic_job_flex_interval_ms,
  num_reschedules_due_to_abandonment,
  back_off_policy_type,
  lead_internal_stop_reason AS internal_stop_reason,
  lead_public_stop_reason AS public_stop_reason,
  has_charging_constraint,
  has_battery_not_low_constraint,
  has_storage_not_low_constraint,
  has_timing_delay_constraint,
  has_deadline_constraint,
  has_idle_constraint,
  has_connectivity_constraint,
  has_content_trigger_constraint,
  is_requested_expedited_job,
  is_running_as_expedited_job,
  is_prefetch,
  is_requested_as_user_initiated_job,
  is_running_as_user_initiated_job,
  is_periodic,
  has_flexibility_constraint,
  can_apply_transport_affinities
FROM _android_js_job_started;
