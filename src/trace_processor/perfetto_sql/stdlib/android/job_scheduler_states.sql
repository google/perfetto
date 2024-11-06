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

INCLUDE PERFETTO MODULE counters.intervals;
INCLUDE PERFETTO MODULE android.battery.charging_states;
INCLUDE PERFETTO MODULE intervals.intersect;

CREATE PERFETTO TABLE _screen_states AS
SELECT
  id,
  ts,
  dur,
  screen_state
FROM (
  WITH _screen_state_span AS (
  SELECT *
  FROM counter_leading_intervals!((
    SELECT counter.id, ts, 0 AS track_id, value
    FROM counter
    JOIN counter_track ON counter_track.id = counter.track_id
    WHERE name = 'ScreenState'
  ))) SELECT
    id,
    ts,
    dur,
    CASE value
      WHEN 1 THEN 'Screen off'
      WHEN 2 THEN 'Screen on'
      WHEN 3 THEN 'Always-on display (doze)'
      ELSE 'Unknown'
      END AS screen_state
    FROM _screen_state_span
    WHERE dur > 0
    -- Either the above select statement is populated or the
    -- select statement after the union is populated but not both.
    UNION
     -- When the trace does not have a slice in the screen state track then
    -- we will assume that the screen state for the entire trace is Unknown.
    -- This ensures that we still have job data even if the screen state is
    -- not known. The following statement will only ever return a single row.
    SELECT 1, TRACE_START() as ts, TRACE_DUR() as dur, 'Unknown'
    WHERE NOT EXISTS (
      SELECT * FROM _screen_state_span
    ) AND TRACE_DUR() > 0
);

CREATE PERFETTO TABLE _job_states AS
SELECT
  t.id as track_id,
  s.ts,
  s.id AS slice_id,
  extract_arg(arg_set_id, 'scheduled_job_state_changed.job_name') AS job_name,
  extract_arg(arg_set_id, 'scheduled_job_state_changed.attribution_node[0].uid') AS uid,
  extract_arg(arg_set_id, 'scheduled_job_state_changed.state') AS state,
  extract_arg(arg_set_id, 'scheduled_job_state_changed.internal_stop_reason')
    AS internal_stop_reason,
  extract_arg(arg_set_id, 'scheduled_job_state_changed.public_stop_reason')
    AS public_stop_reason,
  extract_arg(arg_set_id, 'scheduled_job_state_changed.effective_priority')
    AS effective_priority,
  extract_arg(arg_set_id, 'scheduled_job_state_changed.has_battery_not_low_constraint')
    AS has_battery_not_low_constraint,
  extract_arg(arg_set_id, 'scheduled_job_state_changed.has_charging_constraint')
    AS has_charging_constraint,
  extract_arg(arg_set_id, 'scheduled_job_state_changed.has_connectivity_constraint')
    AS has_connectivity_constraint,
  extract_arg(arg_set_id, 'scheduled_job_state_changed.has_content_trigger_constraint')
    AS has_content_trigger_constraint,
  extract_arg(arg_set_id, 'scheduled_job_state_changed.has_deadline_constraint')
    AS has_deadline_constraint,
  extract_arg(arg_set_id, 'scheduled_job_state_changed.has_idle_constraint')
    AS has_idle_constraint,
  extract_arg(arg_set_id, 'scheduled_job_state_changed.has_storage_not_low_constraint')
    AS has_storage_not_low_constraint,
  extract_arg(arg_set_id, 'scheduled_job_state_changed.has_timing_delay_constraint')
    AS has_timing_delay_constraint,
  extract_arg(arg_set_id, 'scheduled_job_state_changed.is_prefetch') == 1
    AS is_prefetch,
  extract_arg(arg_set_id, 'scheduled_job_state_changed.is_requested_expedited_job')
    AS is_requested_expedited_job,
  extract_arg(arg_set_id, 'scheduled_job_state_changed.is_running_as_expedited_job')
    AS is_running_as_expedited_job,
  extract_arg(arg_set_id, 'scheduled_job_state_changed.job_id') AS job_id,
  extract_arg(arg_set_id, 'scheduled_job_state_changed.num_previous_attempts')
    AS num_previous_attempts,
  extract_arg(arg_set_id, 'scheduled_job_state_changed.requested_priority')
    AS requested_priority,
  extract_arg(arg_set_id, 'scheduled_job_state_changed.standby_bucket')
    AS standby_bucket,
  extract_arg(arg_set_id, 'scheduled_job_state_changed.is_periodic')
    AS is_periodic,
  extract_arg(arg_set_id, 'scheduled_job_state_changed.is_periodic')
    AS has_flex_constraint,
  extract_arg(arg_set_id, 'scheduled_job_state_changed.is_requested_as_user_initiated_job')
    AS is_requested_as_user_initiated_job,
  extract_arg(arg_set_id, 'scheduled_job_state_changed.is_running_as_user_initiated_job')
    AS is_running_as_user_initiated_job,
  extract_arg(arg_set_id, 'scheduled_job_state_changed.deadline_ms')
    AS deadline_ms,
  extract_arg(arg_set_id, 'scheduled_job_state_changed.job_start_latency_ms')
    AS job_start_latency_ms,
  extract_arg(arg_set_id, 'scheduled_job_state_changed.num_uncompleted_work_items')
    AS num_uncompleted_work_items,
  extract_arg(arg_set_id, 'scheduled_job_state_changed.proc_state')
    AS proc_state
FROM
  track t
JOIN slice s
  ON (s.track_id = t.id)
WHERE
  t.name = 'Statsd Atoms' AND s.name = 'scheduled_job_state_changed';

CREATE PERFETTO TABLE _job_started AS
WITH cte AS (
  SELECT
    *,
    LEAD(state, 1)
      OVER (PARTITION BY uid, job_name, job_id ORDER BY uid, job_name, job_id, ts) AS lead_state,
    LEAD(ts, 1, TRACE_END())
      OVER (PARTITION BY uid, job_name, job_id ORDER BY uid, job_name, job_id, ts) AS ts_lead,
    --- Filter out statsd lossy issue.
    LEAD(ts, 1)
      OVER (PARTITION BY uid, job_name, job_id ORDER BY uid, job_name, job_id, ts) IS NULL AS is_end_slice,
    LEAD(internal_stop_reason, 1, 'INTERNAL_STOP_REASON_UNKNOWN')
      OVER (
        PARTITION BY uid, job_name, job_id
        ORDER BY uid, job_name, job_id, ts
      ) AS lead_internal_stop_reason,
    LEAD(public_stop_reason, 1, 'PUBLIC_STOP_REASON_UNKNOWN')
      OVER (
        PARTITION BY uid, job_name, job_id
        ORDER BY uid, job_name, job_id, ts
      ) AS lead_public_stop_reason
  FROM _job_states
  WHERE state != 'CANCELLED'
)
SELECT
  -- Job name is based on whether the tag and/or namespace are present:
  -- 1. Both tag and namespace are present: @<namespace>@<tag>:<package name>
  -- 2. Only tag is present:  <tag>:<package name>
  -- 3. Only namespace is present: @<namespace>@<package name>/<class name>
  CASE
    WHEN substr(job_name, 1, 1) = '@'
      THEN
        CASE
          WHEN substr(STR_SPLIT(job_name, '/', 1), 1, 3) = 'com' THEN STR_SPLIT(job_name, '/', 1)
          ELSE STR_SPLIT(STR_SPLIT(job_name, '/', 0), '@', 2)
          END
    ELSE STR_SPLIT(job_name, '/', 0)
    END AS package_name,
  CASE
    WHEN substr(job_name, 1, 1) = '@' THEN STR_SPLIT(job_name, '@', 1)
    ELSE STR_SPLIT(job_name, '/', 1)
    END AS job_namespace,
  ts_lead - ts AS dur,
  IIF(lead_state = 'SCHEDULED', TRUE, FALSE) AS is_rescheduled,
  *
FROM cte
WHERE
  is_end_slice = FALSE
  AND (ts_lead - ts) > 0
  AND state = 'STARTED'
  AND lead_state IN ('FINISHED', 'SCHEDULED');

CREATE PERFETTO TABLE _charging_screen_states AS
SELECT
  ROW_NUMBER() OVER () AS id,
  ii.ts,
  ii.dur,
  c.charging_state,
  s.screen_state
FROM _interval_intersect!(
  (android_charging_states, _screen_states),
  ()
) ii
JOIN android_charging_states c ON c.id = ii.id_0
JOIN _screen_states s ON s.id = ii.id_1;

-- This table returns constraint changes that a
-- job will go through in a single trace.
--
-- Values in this table are derived from the the `ScheduledJobStateChanged`
-- atom. This table differs from the
-- `android_job_scheduler_with_screen_charging_states` in this module
-- (`android.job_scheduler_states`) by only having job constraint information.
--
-- See documentation for the `android_job_scheduler_with_screen_charging_states`
-- for how tables in this module differ from `android_job_scheduler_events`
-- table in the `android.job_scheduler` module and how to populate this table.
CREATE PERFETTO TABLE android_job_scheduler_states(
  -- Unique identifier for row.
  id INT,
  -- Timestamp of job state slice.
  ts INT,
  -- Duration of job state slice.
  dur INT,
  -- Id of the slice.
  slice_id INT,
  -- Name of the job (as named by the app).
  job_name STRING,
  -- Uid associated with job.
  uid INT,
  -- Id of job (assigned by app for T- builds and system generated in U+
  -- builds).
  job_id INT,
  -- Package that the job belongs (ex: associated app).
  package_name STRING,
  -- Namespace of job.
  job_namespace STRING,
  -- Priority at which JobScheduler ran the job.
  effective_priority INT,
  -- True if app requested job should run when the device battery is not low.
  has_battery_not_low_constraint BOOL,
  -- True if app requested job should run when the device is charging.
  has_charging_constraint BOOL,
  -- True if app requested job should run when device has connectivity.
  has_connectivity_constraint BOOL,
  -- True if app requested job should run when there is a content trigger.
  has_content_trigger_constraint BOOL,
  -- True if app requested there is a deadline by which the job should run.
  has_deadline_constraint BOOL,
  -- True if app requested job should run when device is idle.
  has_idle_constraint BOOL,
  -- True if app requested job should run when device storage is not low.
  has_storage_not_low_constraint BOOL,
  -- True if app requested job has a timing delay.
  has_timing_delay_constraint BOOL,
  -- True if app requested job should run within hours of app launch.
  is_prefetch BOOL,
  -- True if app requested that the job is run as an expedited job.
  is_requested_expedited_job BOOL,
  -- The job is run as an expedited job.
  is_running_as_expedited_job BOOL,
  -- Number of previous attempts at running job.
  num_previous_attempts INT,
  -- The requested priority at which the job should run.
  requested_priority INT,
  -- The job's standby bucket (one of: Active, Working Set, Frequent, Rare,
  -- Never, Restricted, Exempt).
  standby_bucket STRING,
  -- Job should run in intervals.
  is_periodic BOOL,
  -- True if the job should run as a flex job.
  has_flex_constraint BOOL,
  -- True is app has requested that a job be run as a user initiated job.
  is_requested_as_user_initiated_job BOOL,
  -- True if job is running as a user initiated job.
  is_running_as_user_initiated_job BOOL,
  -- Deadline that job has requested and valid if has_deadline_constraint is
  -- true.
  deadline_ms INT,
  -- The latency in ms between when a job is scheduled and when it actually
  -- starts.
  job_start_latency_ms INT,
  -- Number of uncompleted job work items.
  num_uncompleted_work_items INT,
  -- Process state of the process responsible for running the job.
  proc_state STRING,
  -- Internal stop reason for a job.
  internal_stop_reason STRING,
  -- Public stop reason for a job.
  public_stop_reason STRING

) AS
SELECT
  ROW_NUMBER() OVER (ORDER BY ts) AS id,
  ts,
  dur,
  slice_id,
  job_name,
  uid,
  job_id,
  package_name,
  job_namespace,
  effective_priority,
  has_battery_not_low_constraint,
  has_charging_constraint,
  has_connectivity_constraint,
  has_content_trigger_constraint,
  has_deadline_constraint,
  has_idle_constraint,
  has_storage_not_low_constraint,
  has_timing_delay_constraint,
  is_prefetch,
  is_requested_expedited_job,
  is_running_as_expedited_job,
  num_previous_attempts,
  requested_priority,
  standby_bucket,
  is_periodic,
  has_flex_constraint,
  is_requested_as_user_initiated_job,
  is_running_as_user_initiated_job,
  deadline_ms,
  job_start_latency_ms,
  num_uncompleted_work_items,
  proc_state,
  lead_internal_stop_reason AS internal_stop_reason,
  lead_public_stop_reason AS public_stop_reason
FROM _job_started;

-- This table returns the constraint, charging,
-- and screen state changes that a job will go through
-- in a single trace.
--
-- Values from this table are derived from
-- the `ScheduledJobStateChanged` atom. This differs from the
-- `android_job_scheduler_events` table in the `android.job_scheduler` module
-- which is derived from ATrace the system server category
-- (`atrace_categories: "ss"`).
--
-- This also differs from the `android_job_scheduler_states` in this module
-- (`android.job_scheduler_states`) by providing charging and screen state
-- changes.
--
-- To populate this table, enable the Statsd Tracing Config with the
-- ATOM_SCHEDULED_JOB_STATE_CHANGED push atom id.
-- https://perfetto.dev/docs/reference/trace-config-proto#StatsdTracingConfig
--
-- This table is preferred over `android_job_scheduler_events`
-- since it contains more information and should be used whenever
-- `ATOM_SCHEDULED_JOB_STATE_CHANGED` is available in a trace.
CREATE PERFETTO TABLE android_job_scheduler_with_screen_charging_states(
  -- Timestamp of job.
  ts INT,
  -- Duration of slice in ns.
  dur INT,
  -- Id of the slice.
  slice_id INT,
  -- Name of the job (as named by the app).
  job_name STRING,
  -- Id of job (assigned by app for T- builds and system generated in U+
  -- builds).
  job_id INT,
  -- Uid associated with job.
  uid INT,
  -- Duration of entire job in ns.
  job_dur INT,
  -- Package that the job belongs (ex: associated app).
  package_name STRING,
  -- Namespace of job.
  job_namespace STRING,
  -- Device charging state during job (one of: Charging, Discharging, Not charging,
  -- Full, Unknown).
  charging_state STRING,
  -- Device screen state during job (one of: Screen off, Screen on, Always-on display
  -- (doze), Unknown).
  screen_state STRING,
  -- Priority at which JobScheduler ran the job.
  effective_priority INT,
  -- True if app requested job should run when the device battery is not low.
  has_battery_not_low_constraint BOOL,
  -- True if app requested job should run when the device is charging.
  has_charging_constraint BOOL,
  -- True if app requested job should run when device has connectivity.
  has_connectivity_constraint BOOL,
  -- True if app requested job should run when there is a content trigger.
  has_content_trigger_constraint BOOL,
  -- True if app requested there is a deadline by which the job should run.
  has_deadline_constraint BOOL,
  -- True if app requested job should run when device is idle.
  has_idle_constraint BOOL,
  -- True if app requested job should run when device storage is not low.
  has_storage_not_low_constraint BOOL,
  -- True if app requested job has a timing delay.
  has_timing_delay_constraint BOOL,
  -- True if app requested job should run within hours of app launch.
  is_prefetch BOOL,
  -- True if app requested that the job is run as an expedited job.
  is_requested_expedited_job BOOL,
  -- The job is run as an expedited job.
  is_running_as_expedited_job BOOL,
  -- Number of previous attempts at running job.
  num_previous_attempts INT,
  -- The requested priority at which the job should run.
  requested_priority INT,
  -- The job's standby bucket (one of: Active, Working Set, Frequent, Rare,
  -- Never, Restricted, Exempt).
  standby_bucket STRING,
  -- Job should run in intervals.
  is_periodic BOOL,
  -- True if the job should run as a flex job.
  has_flex_constraint BOOL,
  -- True is app has requested that a job be run as a user initiated job.
  is_requested_as_user_initiated_job BOOL,
  -- True if job is running as a user initiated job.
  is_running_as_user_initiated_job BOOL,
  -- Deadline that job has requested and valid if has_deadline_constraint is
  -- true.
  deadline_ms INT,
  -- The latency in ms between when a job is scheduled and when it actually
  -- starts.
  job_start_latency_ms INT,
  -- Number of uncompleted job work items.
  num_uncompleted_work_items INT,
  -- Process state of the process responsible for running the job.
  proc_state STRING,
  -- Internal stop reason for a job.
  internal_stop_reason STRING,
  -- Public stop reason for a job.
  public_stop_reason STRING
) AS
SELECT
  ii.ts,
  ii.dur,
  js.slice_id,
  js.job_name || '_' || js.job_id AS job_name,
  js.uid,
  js.job_id,
  js.dur AS job_dur,
  js.package_name,
  js.job_namespace,
  c.charging_state,
  c.screen_state,
  js.effective_priority,
  js.has_battery_not_low_constraint,
  js.has_charging_constraint,
  js.has_connectivity_constraint,
  js.has_content_trigger_constraint,
  js.has_deadline_constraint,
  js.has_idle_constraint,
  js.has_storage_not_low_constraint,
  js.has_timing_delay_constraint,
  js.is_prefetch,
  js.is_requested_expedited_job,
  js.is_running_as_expedited_job,
  js.num_previous_attempts,
  js.requested_priority,
  js.standby_bucket,
  js.is_periodic,
  js.has_flex_constraint,
  js.is_requested_as_user_initiated_job,
  js.is_running_as_user_initiated_job,
  js.deadline_ms,
  js.job_start_latency_ms,
  js.num_uncompleted_work_items,
  js.proc_state,
  js.internal_stop_reason,
  js.public_stop_reason
  FROM _interval_intersect!(
        (_charging_screen_states,
        android_job_scheduler_states),
        ()
      ) ii
  JOIN _charging_screen_states c ON c.id = ii.id_0
  JOIN android_job_scheduler_states js ON js.id = ii.id_1;
