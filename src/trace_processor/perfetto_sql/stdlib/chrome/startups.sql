-- Copyright 2023 The Chromium Authors
-- Use of this source code is governed by a BSD-style license that can be
-- found in the LICENSE file.

INCLUDE PERFETTO MODULE slices.with_context;

-- Chrome startups, including launch cause.
CREATE PERFETTO PIPELINE chrome_startups (
  -- Unique ID
  id LONG,
  -- Chrome Activity event id of the launch.
  activity_id LONG,
  -- Name of the launch start event.
  name STRING,
  -- Timestamp that the startup occurred.
  startup_begin_ts TIMESTAMP,
  -- Timestamp to the first visible content.
  first_visible_content_ts TIMESTAMP,
  -- Launch cause. See Startup.LaunchCauseType in chrome_track_event.proto.
  launch_cause STRING,
  -- Process ID of the Browser where the startup occurred.
  browser_upid LONG
)
MATERIALIZED AS
-- Access all startups, including those that don't lead to any visible content.
-- If TimeToFirstVisibleContent is available, then this event will be the
-- main event of the startup. Otherwise, the event for the start timestamp will
-- be used.
SUBPIPELINE starts AS (
  FROM thread_slice
  |> WHERE name = 'Startup.ActivityStart'
  |> SELECT
    name,
    extract_arg(arg_set_id, 'startup.activity_id') AS activity_id,
    ts,
    dur,
    upid AS browser_upid
)
SUBPIPELINE times_to_first_visible_content AS (
  FROM process_slice
  |> WHERE name = 'Startup.TimeToFirstVisibleContent2'
  |> SELECT
    name,
    extract_arg(arg_set_id, 'startup.activity_id') AS activity_id,
    ts,
    dur,
    upid AS browser_upid
)
SUBPIPELINE activity_ids AS (
  FROM starts
  |> SELECT DISTINCT activity_id, browser_upid
  |> UNION ALL (
    FROM times_to_first_visible_content
    |> SELECT DISTINCT activity_id, browser_upid
  )
  |> SELECT DISTINCT activity_id, browser_upid
)
SUBPIPELINE start_events AS (
  FROM activity_ids
  |> LEFT JOIN times_to_first_visible_content
    USING (activity_id, browser_upid)
  |> LEFT JOIN starts
    USING (activity_id, browser_upid)
  |> SELECT
    activity_ids.activity_id,
    'Startup' AS name,
    coalesce(times_to_first_visible_content.ts, starts.ts) AS startup_begin_ts,
    times_to_first_visible_content.ts + times_to_first_visible_content.dur AS first_visible_content_ts,
    activity_ids.browser_upid
)
-- Chrome launch causes, not recorded at start time; use the activity id to
-- join with the actual startup events.
SUBPIPELINE launches AS (
  FROM thread_slice
  |> WHERE name = 'Startup.LaunchCause'
  |> SELECT
    extract_arg(arg_set_id, 'startup.activity_id') AS activity_id,
    extract_arg(arg_set_id, 'startup.launch_cause') AS launch_cause,
    upid AS browser_upid
)
FROM start_events
|> LEFT JOIN launches USING (activity_id, browser_upid)
|> SELECT
  row_number() OVER (ORDER BY start_events.startup_begin_ts) AS id,
  start_events.activity_id,
  start_events.name,
  start_events.startup_begin_ts,
  start_events.first_visible_content_ts,
  launches.launch_cause,
  start_events.browser_upid;
