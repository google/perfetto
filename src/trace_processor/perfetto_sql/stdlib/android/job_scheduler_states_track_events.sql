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

-- Extracts trace tag from job name.
-- Expected format: Contains '#<tag>#', e.g., "#my_tag#com.example.job.Service"
-- Returns the extracted tag, or empty string if not found.
CREATE PERFETTO FUNCTION _android_js_extract_trace_tag(job_name STRING)
RETURNS STRING
AS
SELECT coalesce(regexp_extract($job_name, '#(.*)#'), '');

-- Extracts package name from job name.
-- Expected formats:
-- 1. "package_name/class_name" (standard JobInfo format)
-- 2. "class_name:package_name" (alternative format used in some contexts)
-- Returns the extracted package name, or empty string if not found.
CREATE PERFETTO FUNCTION _android_js_extract_package_name(job_name STRING)
RETURNS STRING
AS
SELECT
  coalesce(
    regexp_extract($job_name, '([a-zA-Z0-9_.-]+)/'),
    regexp_extract($job_name, ':([a-zA-Z0-9_.-]*)$')
  );

-- Extracts namespace from job name.
-- Expected format: Contains '@<namespace>@', e.g., "@my_namespace@com.example.job.Service"
-- Returns the extracted namespace, or empty string if not found.
CREATE PERFETTO FUNCTION _android_js_extract_job_namespace(job_name STRING)
RETURNS STRING
AS
SELECT coalesce(regexp_extract($job_name, '@(.*)@'), '');

-- Create a table containing all JobScheduler job state changes from slices.
CREATE PERFETTO TABLE _android_js_job_state_events AS
SELECT
  js.ts AS ts,
  CASE
    WHEN js.job_name IS NULL
    OR js.job_name = '' THEN s.name
    ELSE js.job_name
  END AS job_name,
  js.slice_id AS slice_id,
  js.job_id AS job_id,
  js.uid AS uid,
  js.proxy_uid AS proxy_uid,
  js.state AS state,
  js.standby_bucket AS standby_bucket,
  js.requested_priority AS requested_priority,
  js.effective_priority AS effective_priority,
  js.num_previous_attempts AS num_previous_attempts,
  js.deadline_ms AS deadline_ms,
  js.delay_ms AS delay_ms,
  js.job_start_latency_ms AS job_start_latency_ms,
  js.num_uncompleted_work_items AS num_uncompleted_work_items,
  js.proc_state AS proc_state,
  js.internal_stop_reason AS internal_stop_reason,
  js.public_stop_reason AS public_stop_reason,
  js.periodic_job_interval_ms AS periodic_job_interval_ms,
  js.periodic_job_flex_interval_ms AS periodic_job_flex_interval_ms,
  _android_js_extract_trace_tag(js.job_name) AS filtered_trace_tag,
  js.num_reschedules_due_to_abandonment AS num_reschedules_due_to_abandonment,
  js.back_off_policy_type AS back_off_policy_type,
  CAST(js.has_charging_constraint AS BOOL) AS has_charging_constraint,
  CAST(js.has_battery_not_low_constraint AS BOOL) AS has_battery_not_low_constraint,
  CAST(js.has_storage_not_low_constraint AS BOOL) AS has_storage_not_low_constraint,
  CAST(js.has_timing_delay_constraint AS BOOL) AS has_timing_delay_constraint,
  CAST(js.has_deadline_constraint AS BOOL) AS has_deadline_constraint,
  CAST(js.has_idle_constraint AS BOOL) AS has_idle_constraint,
  CAST(js.has_connectivity_constraint AS BOOL) AS has_connectivity_constraint,
  CAST(js.has_content_trigger_constraint AS BOOL) AS has_content_trigger_constraint,
  CAST(js.is_requested_expedited_job AS BOOL) AS is_requested_expedited_job,
  CAST(js.is_running_as_expedited_job AS BOOL) AS is_running_as_expedited_job,
  CAST(js.is_prefetch AS BOOL) AS is_prefetch,
  CAST(js.is_requested_as_user_initiated_job AS BOOL) AS is_requested_as_user_initiated_job,
  CAST(js.is_running_as_user_initiated_job AS BOOL) AS is_running_as_user_initiated_job,
  CAST(js.is_periodic AS BOOL) AS is_periodic,
  CAST(js.has_flexibility_constraint AS BOOL) AS has_flexibility_constraint,
  CAST(js.can_apply_transport_affinities AS BOOL) AS can_apply_transport_affinities
FROM __intrinsic_android_job_scheduler_track_events AS js
JOIN slice AS s
  ON js.slice_id = s.id;

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
    WINDOW
      job_asc AS (PARTITION BY uid, job_name, job_id ORDER BY ts)
  )
SELECT
  _android_js_extract_package_name(job_name) AS package_name,
  _android_js_extract_job_namespace(job_name) AS job_namespace,
  ts_lead - ts AS dur,
  iif(lead_state = 'SCHEDULED', TRUE, FALSE) AS is_rescheduled,
  coalesce(
    nullif(requested_priority, 'PRIORITY_UNKNOWN'),
    lag_requested_priority
  ) AS effective_requested_priority,
  *
FROM job_states_with_lead
WHERE
  is_end_slice = FALSE
  AND (ts_lead - ts) > 0
  AND state = 'STARTED'
  AND lead_state IN ('FINISHED', 'SCHEDULED');

-- This table returns all running jobs from job scheduler track events.
--
-- Values in this table are derived from job scheduler track events.
-- To populate this table, the `jobscheduler` category must be enabled
-- in the `track_event` data source.
CREATE PERFETTO TABLE android_job_scheduler_states_track_events(
  -- Unique identifier for job scheduler state.
  id LONG,
  -- Id of the slice.
  slice_id JOINID(slice.id),
  -- Timestamp of job state slice.
  ts TIMESTAMP,
  -- Duration of job state slice.
  dur DURATION,
  -- True if the job was rescheduled to run again.
  is_rescheduled BOOL,
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
)
AS
SELECT
  row_number() OVER (ORDER BY ts) AS id,
  slice_id,
  ts,
  dur,
  is_rescheduled,
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
