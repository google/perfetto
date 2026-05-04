-- Provides unified access to Android JobScheduler events.
--
-- Suggested minimal config:
--
-- data_sources: {
--   config: {
--     name: "linux.ftrace"
--     ftrace_config: {
--       atrace_apps: "*"
--       atrace_categories: "jobscheduler"
--       atrace_categories: "power"
--     }
--   }
-- }
--
-- data_sources: {
--   config: {
--     name: "android.statsd"
--     statsd_config: {
--       # Atom ID for ScheduledJobStateChanged
--       atom_id: 59
--     }
--   }
-- }
INCLUDE PERFETTO MODULE android.battery_stats;

CREATE PERFETTO FUNCTION _has_job_state_flag(
    flags LONG,
    flag_name STRING
)
RETURNS BOOL AS
SELECT
  CASE $flag_name
    WHEN 'HAS_CHARGING_CONSTRAINT'
    THEN (
      $flags & (
        1 << 0
      )
    ) != 0
    WHEN 'HAS_BATTERY_NOT_LOW_CONSTRAINT'
    THEN (
      $flags & (
        1 << 1
      )
    ) != 0
    WHEN 'HAS_STORAGE_NOT_LOW_CONSTRAINT'
    THEN (
      $flags & (
        1 << 2
      )
    ) != 0
    WHEN 'HAS_TIMING_DELAY_CONSTRAINT'
    THEN (
      $flags & (
        1 << 3
      )
    ) != 0
    WHEN 'HAS_DEADLINE_CONSTRAINT'
    THEN (
      $flags & (
        1 << 4
      )
    ) != 0
    WHEN 'HAS_IDLE_CONSTRAINT'
    THEN (
      $flags & (
        1 << 5
      )
    ) != 0
    WHEN 'HAS_CONNECTIVITY_CONSTRAINT'
    THEN (
      $flags & (
        1 << 6
      )
    ) != 0
    WHEN 'HAS_CONTENT_TRIGGER_CONSTRAINT'
    THEN (
      $flags & (
        1 << 7
      )
    ) != 0
    WHEN 'IS_REQUESTED_EXPEDITED_JOB'
    THEN (
      $flags & (
        1 << 8
      )
    ) != 0
    WHEN 'IS_RUNNING_AS_EXPEDITED_JOB'
    THEN (
      $flags & (
        1 << 9
      )
    ) != 0
    WHEN 'IS_PREFETCH'
    THEN (
      $flags & (
        1 << 10
      )
    ) != 0
    WHEN 'IS_REQUESTED_AS_USER_INITIATED_JOB'
    THEN (
      $flags & (
        1 << 19
      )
    ) != 0
    WHEN 'IS_RUNNING_AS_USER_INITIATED_JOB'
    THEN (
      $flags & (
        1 << 20
      )
    ) != 0
    WHEN 'IS_PERIODIC'
    THEN (
      $flags & (
        1 << 21
      )
    ) != 0
    WHEN 'HAS_FLEXIBILITY_CONSTRAINT'
    THEN (
      $flags & (
        1 << 22
      )
    ) != 0
    WHEN 'CAN_APPLY_TRANSPORT_AFFINITIES'
    THEN (
      $flags & (
        1 << 24
      )
    ) != 0
    ELSE FALSE
  END;

-- Converts job state integer to string name.
CREATE PERFETTO FUNCTION _get_job_state_name(
    state LONG
)
RETURNS STRING AS
SELECT
  CASE $state
    WHEN 0
    THEN 'FINISHED'
    WHEN 1
    THEN 'STARTED'
    WHEN 2
    THEN 'SCHEDULED'
    WHEN 3
    THEN 'CANCELLED'
    ELSE 'UNKNOWN'
  END;

-- Converts standby bucket integer to string name.
CREATE PERFETTO FUNCTION _get_standby_bucket_name(
    bucket LONG
)
RETURNS STRING AS
SELECT
  CASE $bucket
    WHEN 0
    THEN 'ACTIVE'
    WHEN 1
    THEN 'WORKING_SET'
    WHEN 2
    THEN 'FREQUENT'
    WHEN 3
    THEN 'RARE'
    WHEN 4
    THEN 'NEVER'
    WHEN 5
    THEN 'RESTRICTED'
    WHEN 6
    THEN 'EXEMPTED'
    ELSE 'UNKNOWN'
  END;

-- Converts internal stop reason integer to string name.
CREATE PERFETTO FUNCTION _get_internal_stop_reason_name(
    reason LONG
)
RETURNS STRING AS
SELECT
  CASE $reason
    WHEN -1
    THEN 'INTERNAL_STOP_REASON_UNKNOWN'
    WHEN 0
    THEN 'INTERNAL_STOP_REASON_CANCELED'
    WHEN 1
    THEN 'INTERNAL_STOP_REASON_CONSTRAINTS_NOT_SATISFIED'
    WHEN 2
    THEN 'INTERNAL_STOP_REASON_PREEMPT'
    WHEN 3
    THEN 'INTERNAL_STOP_REASON_TIMEOUT'
    WHEN 4
    THEN 'INTERNAL_STOP_REASON_DEVICE_IDLE'
    WHEN 5
    THEN 'INTERNAL_STOP_REASON_DEVICE_THERMAL'
    WHEN 6
    THEN 'INTERNAL_STOP_REASON_RESTRICTED_BUCKET'
    WHEN 7
    THEN 'INTERNAL_STOP_REASON_UNINSTALL'
    WHEN 8
    THEN 'INTERNAL_STOP_REASON_DATA_CLEARED'
    WHEN 9
    THEN 'INTERNAL_STOP_REASON_RTC_UPDATED'
    WHEN 10
    THEN 'INTERNAL_STOP_REASON_SUCCESSFUL_FINISH'
    WHEN 11
    THEN 'INTERNAL_STOP_REASON_USER_UI_STOP'
    WHEN 12
    THEN 'INTERNAL_STOP_REASON_ANR'
    WHEN 13
    THEN 'INTERNAL_STOP_REASON_TIMEOUT_ABANDONED'
    ELSE 'INTERNAL_STOP_REASON_UNKNOWN'
  END;

-- Converts public stop reason integer to string name.
CREATE PERFETTO FUNCTION _get_public_stop_reason_name(
    reason LONG
)
RETURNS STRING AS
SELECT
  CASE $reason
    WHEN 0
    THEN 'STOP_REASON_UNDEFINED'
    WHEN 1
    THEN 'STOP_REASON_CANCELLED_BY_APP'
    WHEN 2
    THEN 'STOP_REASON_PREEMPT'
    WHEN 3
    THEN 'STOP_REASON_TIMEOUT'
    WHEN 4
    THEN 'STOP_REASON_DEVICE_STATE'
    WHEN 5
    THEN 'STOP_REASON_CONSTRAINT_BATTERY_NOT_LOW'
    WHEN 6
    THEN 'STOP_REASON_CONSTRAINT_CHARGING'
    WHEN 7
    THEN 'STOP_REASON_CONSTRAINT_CONNECTIVITY'
    WHEN 8
    THEN 'STOP_REASON_CONSTRAINT_DEVICE_IDLE'
    WHEN 9
    THEN 'STOP_REASON_CONSTRAINT_STORAGE_NOT_LOW'
    WHEN 10
    THEN 'STOP_REASON_QUOTA'
    WHEN 11
    THEN 'STOP_REASON_BACKGROUND_RESTRICTION'
    WHEN 12
    THEN 'STOP_REASON_APP_STANDBY'
    WHEN 13
    THEN 'STOP_REASON_USER'
    WHEN 14
    THEN 'STOP_REASON_SYSTEM_PROCESSING'
    WHEN 15
    THEN 'STOP_REASON_ESTIMATED_APP_LAUNCH_TIME_CHANGED'
    WHEN 16
    THEN 'STOP_REASON_TIMEOUT_ABANDONED'
    ELSE 'STOP_REASON_UNDEFINED'
  END;

-- Converts backoff policy integer to string name.
CREATE PERFETTO FUNCTION _get_backoff_policy(
    policy_type LONG
)
RETURNS STRING AS
SELECT
  CASE $policy_type
    WHEN 0
    THEN 'UNKNOWN_POLICY'
    WHEN 1
    THEN 'LINEAR'
    WHEN 2
    THEN 'EXPONENTIAL'
    ELSE 'UNKNOWN_POLICY'
  END;

-- Converts proc state integer to string name.
CREATE PERFETTO FUNCTION _get_proc_state_name(
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
    WHEN 1000
    THEN 'PROCESS_STATE_PERSISTENT'
    WHEN 1001
    THEN 'PROCESS_STATE_PERSISTENT_UI'
    WHEN 1002
    THEN 'PROCESS_STATE_TOP'
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
    WHEN 1018
    THEN 'PROCESS_STATE_CACHED_EMPTY'
    WHEN 1019
    THEN 'PROCESS_STATE_NONEXISTENT'
    WHEN 1020
    THEN 'PROCESS_STATE_BOUND_TOP'
    ELSE 'PROCESS_STATE_UNKNOWN'
  END;

-- Extracts trace tag from job name if job name is in format #tag#...
CREATE PERFETTO FUNCTION _extract_trace_tag(
    job_name STRING
)
RETURNS STRING AS
SELECT
  -- This regex is "greedy": the '.*' will match as many characters as possible
  -- until the final '#' in the pattern can be matched against the last '#'
  -- of the tag section. This correctly captures tags that contain '#'.
  coalesce(regexp_extract($job_name, '#(.*)#'), '');

-- Converts priority integer to string name.
CREATE PERFETTO FUNCTION _get_priority_name(
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

CREATE PERFETTO FUNCTION _extract_package_name(
    job_name STRING
)
RETURNS STRING AS
SELECT
  coalesce(
    -- Case 1: Handles names with a package name before a '/', like
    -- '#TraceTag#com.example/Service' or the SyncManager special case.
    regexp_extract($job_name, '([a-zA-Z0-9_.-]+)/'),
    -- Case 2: Handles names with a package name after the last ':', like
    -- '...:com.example.app'
    regexp_extract($job_name, ':([a-zA-Z0-9_.-]*)$')
  );

-- Extracts job scheduler arg from arg_set_id.
CREATE PERFETTO FUNCTION _extract_js_arg(
    arg_set_id LONG,
    field_name STRING
)
RETURNS STRING AS
SELECT
  extract_arg($arg_set_id, 'job_scheduler_job.' || $field_name);

-- Extracts thermal arg from arg_set_id.
CREATE PERFETTO FUNCTION _extract_thermal_arg(
    arg_set_id LONG,
    field_name STRING
)
RETURNS STRING AS
SELECT
  extract_arg($arg_set_id, 'thermal_throttling_severity_state_changed.' || $field_name);

-- Extracts namespace from job name if job name is in format @namespace@...
CREATE PERFETTO FUNCTION _extract_job_namespace(
    job_name STRING
)
RETURNS STRING AS
SELECT
  -- This regex is "greedy": the '.*' will match as many characters as possible
  -- until the final '@' in the pattern can be matched against the last '@'
  -- of the namespace section. This correctly captures namespaces that contain '@'.
  coalesce(regexp_extract($job_name, '@(.*)@'), '');

-- Create a table containing all JobScheduler job state changes from slices.
CREATE PERFETTO TABLE _job_state_events AS
WITH
  raw_job_states AS (
    SELECT
      s.ts,
      s.id AS slice_id,
      s.name AS job_name,
      s.arg_set_id,
      CAST(_extract_js_arg(s.arg_set_id, 'job_state_flags') AS INTEGER) AS job_state_flags
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
  _extract_js_arg(arg_set_id, 'job_id') AS job_id,
  _extract_js_arg(arg_set_id, 'source_uid') AS uid,
  _extract_js_arg(arg_set_id, 'proxy_uid') AS proxy_uid,
  _get_job_state_name(CAST(_extract_js_arg(arg_set_id, 'state') AS INTEGER)) AS state,
  _get_standby_bucket_name(CAST(_extract_js_arg(arg_set_id, 'standby_bucket') AS INTEGER)) AS standby_bucket,
  _get_priority_name(CAST(_extract_js_arg(arg_set_id, 'requested_priority') AS INTEGER)) AS requested_priority,
  _get_priority_name(CAST(_extract_js_arg(arg_set_id, 'effective_priority') AS INTEGER)) AS effective_priority,
  _extract_js_arg(arg_set_id, 'num_previous_attempts') AS num_previous_attempts,
  _extract_js_arg(arg_set_id, 'deadline_ms') AS deadline_ms,
  _extract_js_arg(arg_set_id, 'delay_ms') AS delay_ms,
  _extract_js_arg(arg_set_id, 'job_start_latency_ms') AS job_start_latency_ms,
  _extract_js_arg(arg_set_id, 'num_uncompleted_work_items') AS num_uncompleted_work_items,
  _get_proc_state_name(CAST(_extract_js_arg(arg_set_id, 'proc_state') AS INTEGER)) AS proc_state,
  _get_internal_stop_reason_name(CAST(_extract_js_arg(arg_set_id, 'internal_stop_reason') AS INTEGER)) AS internal_stop_reason,
  _get_public_stop_reason_name(CAST(_extract_js_arg(arg_set_id, 'public_stop_reason') AS INTEGER)) AS public_stop_reason,
  _extract_js_arg(arg_set_id, 'periodic_job_interval_ms') AS periodic_job_interval_ms,
  _extract_js_arg(arg_set_id, 'periodic_job_flex_interval_ms') AS periodic_job_flex_interval_ms,
  _extract_trace_tag(job_name) AS filtered_trace_tag,
  _extract_js_arg(arg_set_id, 'num_reschedules_due_to_abandonment') AS num_reschedules_due_to_abandonment,
  _get_backoff_policy(CAST(_extract_js_arg(arg_set_id, 'back_off_policy_type') AS INTEGER)) AS back_off_policy_type,
  _has_job_state_flag(job_state_flags, 'HAS_CHARGING_CONSTRAINT') AS has_charging_constraint,
  _has_job_state_flag(job_state_flags, 'HAS_BATTERY_NOT_LOW_CONSTRAINT') AS has_battery_not_low_constraint,
  _has_job_state_flag(job_state_flags, 'HAS_STORAGE_NOT_LOW_CONSTRAINT') AS has_storage_not_low_constraint,
  _has_job_state_flag(job_state_flags, 'HAS_TIMING_DELAY_CONSTRAINT') AS has_timing_delay_constraint,
  _has_job_state_flag(job_state_flags, 'HAS_DEADLINE_CONSTRAINT') AS has_deadline_constraint,
  _has_job_state_flag(job_state_flags, 'HAS_IDLE_CONSTRAINT') AS has_idle_constraint,
  _has_job_state_flag(job_state_flags, 'HAS_CONNECTIVITY_CONSTRAINT') AS has_connectivity_constraint,
  _has_job_state_flag(job_state_flags, 'HAS_CONTENT_TRIGGER_CONSTRAINT') AS has_content_trigger_constraint,
  _has_job_state_flag(job_state_flags, 'IS_REQUESTED_EXPEDITED_JOB') AS is_requested_expedited_job,
  _has_job_state_flag(job_state_flags, 'IS_RUNNING_AS_EXPEDITED_JOB') AS is_running_as_expedited_job,
  _has_job_state_flag(job_state_flags, 'IS_PREFETCH') AS is_prefetch,
  _has_job_state_flag(job_state_flags, 'IS_REQUESTED_AS_USER_INITIATED_JOB') AS is_requested_as_user_initiated_job,
  _has_job_state_flag(job_state_flags, 'IS_RUNNING_AS_USER_INITIATED_JOB') AS is_running_as_user_initiated_job,
  _has_job_state_flag(job_state_flags, 'IS_PERIODIC') AS is_periodic,
  _has_job_state_flag(job_state_flags, 'HAS_FLEXIBILITY_CONSTRAINT') AS has_flexibility_constraint,
  _has_job_state_flag(job_state_flags, 'CAN_APPLY_TRANSPORT_AFFINITIES') AS can_apply_transport_affinities
FROM raw_job_states;

-- Create table with job execution intervals (state='STARTED').
CREATE PERFETTO TABLE _job_started AS
WITH
  job_states_with_lead AS (
    SELECT
      *,
      lead(state, 1) OVER job_asc AS lead_state,
      lead(ts, 1, trace_end()) OVER job_asc AS ts_lead,
      lead(ts, 1) OVER job_asc IS NULL AS is_end_slice,
      lead(internal_stop_reason, 1, 'INTERNAL_STOP_REASON_UNKNOWN') OVER job_asc AS lead_internal_stop_reason,
      lead(public_stop_reason, 1, 'PUBLIC_STOP_REASON_UNKNOWN') OVER job_asc AS lead_public_stop_reason
    FROM _job_state_events
    WHERE
      state != 'CANCELLED'
    WINDOW job_asc AS (PARTITION BY uid, job_name, job_id ORDER BY ts)
  )
SELECT
  _extract_package_name(job_name) AS package_name,
  _extract_job_namespace(job_name) AS job_namespace,
  ts_lead - ts AS dur,
  iif(lead_state = 'SCHEDULED', TRUE, FALSE) AS is_rescheduled,
  *
FROM job_states_with_lead
WHERE
  is_end_slice = FALSE
  AND (
    ts_lead - ts
  ) > 0
  AND state = 'STARTED'
  AND lead_state IN ('FINISHED', 'SCHEDULED');

-- 1. Table for JobScheduler events sourced from Perfetto SDK (android_job_scheduler_sdk).
-- This table returns all running jobs from job scheduler track events.
--
-- Values in this table are derived from job scheduler track events.
-- This table differs from the
-- `android_job_scheduler_statsd` and `android_job_scheduler_batterystats`
-- in this module by only having job constraint information.
--
CREATE PERFETTO TABLE android_job_scheduler_sdk (
  -- Unique identifier for job scheduler state, taken from slice.id.
  id ID,
  -- Id of the slice.
  slice_id JOINID(slice.id),
  -- Timestamp of job state slice.
  ts TIMESTAMP,
  -- Duration of job state slice.
  dur DURATION,
  -- Duration of the job. If -1, clamped to trace end.
  safe_dur DURATION,
  -- Name of the job (as named by the app).
  job_name STRING,
  -- Package that the job belongs (ex: associated app).
  package_name STRING,
  -- Namespace of job.
  job_namespace STRING,
  -- Id of job (assigned by app for T- builds and system generated in U+
  -- builds).
  job_id LONG,
  -- Uid associated with job.
  uid LONG,
  -- Uid associated with proxy job.
  proxy_uid LONG,
  -- Trace tag set via JobInfo.Builder.setTraceTag(). Basic PII filtering has
  -- been applied,
  -- but further filtering should be done by clients.
  filtered_trace_tag STRING,
  -- The job's standby bucket (one of: Active, Working Set, Frequent, Rare,
  -- Never, Restricted, Exempt).
  standby_bucket STRING,
  -- The requested priority at which the job should run.
  requested_priority STRING,
  -- The effective priority at which the job ran.
  effective_priority STRING,
  -- Number of previous attempts at running job.
  num_previous_attempts LONG,
  -- Deadline that job has requested and valid if has_deadline_constraint is
  -- true.
  deadline_ms LONG,
  -- The delay that the Job has requested.
  -- This is only valid if has_timing_delay_constraint is true.
  delay_ms LONG,
  -- The latency in ms between when a job is scheduled and when it actually
  -- starts.
  job_start_latency_ms LONG,
  -- Number of uncompleted job work items.
  num_uncompleted_work_items LONG,
  -- Process state of the process responsible for running the job.
  proc_state STRING,
  -- Interval for the job to recur when it is set as periodic.
  periodic_job_interval_ms LONG,
  -- Flex interval for the periodic job. This value is set via the second
  -- parameter of JobInfo.Builder.setPeriodic(long, long). The job can
  -- execute at any time in a window flex length at the end of the period.
  -- Trace tag set via JobInfo.Builder.setTraceTag(). Basic PII filtering has
  -- been applied,
  -- but further filtering should be done by clients.
  periodic_job_flex_interval_ms LONG,
  -- Number of reschedules due to job being abandoned.
  num_reschedules_due_to_abandonment LONG,
  -- Back off policy applied to the job that gets rescheduled.
  -- The internal reason a job has stopped.
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
  -- Whether transport preference logic can be applied to this job with flex
  -- policy
  can_apply_transport_affinities BOOL,
  -- Uptime of the device in minutes.
  uptime_mins LONG
) AS
SELECT
  slice_id AS id,
  slice_id,
  ts,
  dur,
  iif(dur = -1, trace_end() - ts, dur) AS safe_dur,
  job_name,
  package_name,
  job_namespace,
  job_id,
  uid,
  proxy_uid,
  filtered_trace_tag,
  standby_bucket,
  requested_priority,
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
  can_apply_transport_affinities,
  CAST((
    trace_start() / (
      1e9 * 60.0
    )
  ) AS INTEGER) AS uptime_mins
FROM _job_started;

-- Jobs, reported by statsd.
CREATE PERFETTO TABLE _jobs_from_statsd AS
WITH
  events AS (
    SELECT
      s.ts,
      extract_arg(s.arg_set_id, 'scheduled_job_state_changed.attribution_node[0].uid') AS uid,
      extract_arg(s.arg_set_id, 'scheduled_job_state_changed.job_name') AS job_name,
      extract_arg(s.arg_set_id, 'scheduled_job_state_changed.state') AS state
    FROM slice AS s
    WHERE
      s.name = 'scheduled_job_state_changed'
  ),
  windowed AS (
    SELECT
      ts,
      uid,
      job_name,
      state,
      lead(ts) OVER (PARTITION BY uid, job_name ORDER BY ts) - ts AS dur
    FROM events
    WHERE
      state IN ('STARTED', 'FINISHED')
  )
SELECT
  ts,
  dur,
  uid,
  job_name
FROM windowed
WHERE
  state = 'STARTED'
ORDER BY
  ts;

-- 2. Table for JobScheduler events sourced from StatsD atoms (_jobs_from_statsd).
-- This is a fallback for traces that do not contain SDK events.
CREATE PERFETTO TABLE android_job_scheduler_statsd (
  -- Timestamp of job state slice.
  ts TIMESTAMP,
  -- Duration of job state slice.
  dur DURATION,
  -- Duration of the job. If -1, clamped to trace end.
  safe_dur DURATION,
  -- Name of the job (as named by the app).
  job_name STRING,
  -- Package that the job belongs (ex: associated app).
  package_name STRING,
  -- Namespace of job.
  job_namespace STRING,
  -- Id of job (assigned by app for T- builds and system generated in U+ builds).
  job_id LONG,
  -- Uid associated with job.
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
  -- The latency in ms between when a job is scheduled and when it actually starts.
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
  -- Whether transport preference logic can be applied to this job with flex policy
  can_apply_transport_affinities BOOL
) AS
SELECT
  ts,
  dur,
  iif(dur = -1, trace_end() - ts, dur) AS safe_dur,
  job_name,
  NULL AS package_name,
  NULL AS job_namespace,
  NULL AS job_id,
  uid,
  NULL AS proxy_uid,
  NULL AS filtered_trace_tag,
  NULL AS standby_bucket,
  NULL AS requested_priority,
  NULL AS effective_priority,
  NULL AS num_previous_attempts,
  NULL AS deadline_ms,
  NULL AS delay_ms,
  NULL AS job_start_latency_ms,
  NULL AS num_uncompleted_work_items,
  NULL AS proc_state,
  NULL AS periodic_job_interval_ms,
  NULL AS periodic_job_flex_interval_ms,
  NULL AS num_reschedules_due_to_abandonment,
  NULL AS back_off_policy_type,
  NULL AS internal_stop_reason,
  NULL AS public_stop_reason,
  NULL AS has_charging_constraint,
  NULL AS has_battery_not_low_constraint,
  NULL AS has_storage_not_low_constraint,
  NULL AS has_timing_delay_constraint,
  NULL AS has_deadline_constraint,
  NULL AS has_idle_constraint,
  NULL AS has_connectivity_constraint,
  NULL AS has_content_trigger_constraint,
  NULL AS is_requested_expedited_job,
  NULL AS is_running_as_expedited_job,
  NULL AS is_prefetch,
  NULL AS is_requested_as_user_initiated_job,
  NULL AS is_running_as_user_initiated_job,
  NULL AS is_periodic,
  NULL AS has_flexibility_constraint,
  NULL AS can_apply_transport_affinities
FROM _jobs_from_statsd;

-- 3. Table for JobScheduler events sourced from BatteryStats ATrace.
-- This is a fallback for traces that do not contain SDK or StatsD events.
CREATE PERFETTO TABLE android_job_scheduler_batterystats (
  -- Timestamp of job state slice.
  ts TIMESTAMP,
  -- Duration of job state slice.
  dur DURATION,
  -- Duration of the job. If -1, clamped to trace end.
  safe_dur DURATION,
  -- Name of the job (as named by the app).
  job_name STRING,
  -- Package that the job belongs (ex: associated app).
  package_name STRING,
  -- Namespace of job.
  job_namespace STRING,
  -- Id of job (assigned by app for T- builds and system generated in U+ builds).
  job_id LONG,
  -- Uid associated with job.
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
  -- The latency in ms between when a job is scheduled and when it actually starts.
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
  -- Whether transport preference logic can be applied to this job with flex policy
  can_apply_transport_affinities BOOL
) AS
SELECT
  ts,
  dur,
  safe_dur,
  str_value AS job_name,
  NULL AS package_name,
  NULL AS job_namespace,
  NULL AS job_id,
  int_value AS uid,
  NULL AS proxy_uid,
  NULL AS filtered_trace_tag,
  NULL AS standby_bucket,
  NULL AS requested_priority,
  NULL AS effective_priority,
  NULL AS num_previous_attempts,
  NULL AS deadline_ms,
  NULL AS delay_ms,
  NULL AS job_start_latency_ms,
  NULL AS num_uncompleted_work_items,
  NULL AS proc_state,
  NULL AS periodic_job_interval_ms,
  NULL AS periodic_job_flex_interval_ms,
  NULL AS num_reschedules_due_to_abandonment,
  NULL AS back_off_policy_type,
  NULL AS internal_stop_reason,
  NULL AS public_stop_reason,
  NULL AS has_charging_constraint,
  NULL AS has_battery_not_low_constraint,
  NULL AS has_storage_not_low_constraint,
  NULL AS has_timing_delay_constraint,
  NULL AS has_deadline_constraint,
  NULL AS has_idle_constraint,
  NULL AS has_connectivity_constraint,
  NULL AS has_content_trigger_constraint,
  NULL AS is_requested_expedited_job,
  NULL AS is_running_as_expedited_job,
  NULL AS is_prefetch,
  NULL AS is_requested_as_user_initiated_job,
  NULL AS is_running_as_user_initiated_job,
  NULL AS is_periodic,
  NULL AS has_flexibility_constraint,
  NULL AS can_apply_transport_affinities
FROM android_battery_stats_event_slices
WHERE
  track_name = 'battery_stats.job';

-- Unified view for JobScheduler events.
-- Prioritizes SDK > StatsD > BatteryStats. If a higher-priority source
-- exists in the trace, only its events will be returned.
CREATE PERFETTO VIEW android_job_scheduler (
  -- Timestamp of job state slice.
  ts TIMESTAMP,
  -- Duration of job state slice.
  dur DURATION,
  -- Duration of the job. If -1, clamped to trace end.
  safe_dur DURATION,
  -- Name of the job (as named by the app).
  job_name STRING,
  -- Package that the job belongs (ex: associated app).
  package_name STRING,
  -- Namespace of job.
  job_namespace STRING,
  -- Id of job (assigned by app for T- builds and system generated in U+ builds).
  job_id LONG,
  -- Uid associated with job.
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
  -- The latency in ms between when a job is scheduled and when it actually starts.
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
  -- Whether transport preference logic can be applied to this job with flex policy
  can_apply_transport_affinities BOOL,
  -- Which underlying data source this row comes from (sdk, statsd, or batterystats).
  data_source STRING
) AS
-- 1. Select from SDK if it exists
SELECT
  ts,
  dur,
  safe_dur,
  job_name,
  package_name,
  job_namespace,
  job_id,
  uid,
  proxy_uid,
  filtered_trace_tag,
  standby_bucket,
  requested_priority,
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
  internal_stop_reason,
  public_stop_reason,
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
  can_apply_transport_affinities,
  'sdk' AS data_source
FROM android_job_scheduler_sdk
UNION ALL
-- 2. Fallback to StatsD if SDK does not exist
SELECT
  ts,
  dur,
  safe_dur,
  job_name,
  package_name,
  job_namespace,
  job_id,
  uid,
  proxy_uid,
  filtered_trace_tag,
  standby_bucket,
  requested_priority,
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
  internal_stop_reason,
  public_stop_reason,
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
  can_apply_transport_affinities,
  'statsd' AS data_source
FROM android_job_scheduler_statsd
WHERE
  NOT EXISTS(
    SELECT
      1
    FROM android_job_scheduler_sdk
  )
UNION ALL
-- 3. Fallback to BatteryStats if SDK and StatsD do not exist
SELECT
  ts,
  dur,
  safe_dur,
  job_name,
  package_name,
  job_namespace,
  job_id,
  uid,
  proxy_uid,
  filtered_trace_tag,
  standby_bucket,
  requested_priority,
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
  internal_stop_reason,
  public_stop_reason,
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
  can_apply_transport_affinities,
  'batterystats' AS data_source
FROM android_job_scheduler_batterystats
WHERE
  NOT EXISTS(
    SELECT
      1
    FROM android_job_scheduler_sdk
  )
  AND NOT EXISTS(
    SELECT
      1
    FROM android_job_scheduler_statsd
  );
