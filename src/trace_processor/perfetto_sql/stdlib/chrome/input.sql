-- Copyright 2024 The Chromium Authors
-- Use of this source code is governed by a BSD-style license that can be
-- found in the LICENSE file.

INCLUDE PERFETTO MODULE slices.with_context;
INCLUDE PERFETTO MODULE chrome.android_input;

-- Processing steps of the Chrome input pipeline.
CREATE PERFETTO TABLE _chrome_input_pipeline_steps_no_input_type(
  -- Id of this Chrome input pipeline (LatencyInfo).
  latency_id LONG,
  -- Slice id
  slice_id LONG,
  -- The step timestamp.
  ts TIMESTAMP,
  -- Step duration.
  dur DURATION,
  -- Utid of the thread.
  utid LONG,
  -- Step name (ChromeLatencyInfo.step).
  step STRING,
  -- Input type.
  input_type STRING,
  -- Start time of the parent Chrome scheduler task (if any) of this step.
  task_start_time_ts TIMESTAMP
) AS
SELECT
  EXTRACT_ARG(thread_slice.arg_set_id, 'chrome_latency_info.trace_id') AS latency_id,
  id AS slice_id,
  ts,
  dur,
  utid,
  EXTRACT_ARG(thread_slice.arg_set_id, 'chrome_latency_info.step') AS step,
  EXTRACT_ARG(thread_slice.arg_set_id, 'chrome_latency_info.input_type') AS input_type,
  ts - (EXTRACT_ARG(thread_slice.arg_set_id, 'current_task.event_offset_from_task_start_time_us') * 1000) AS task_start_time_ts
FROM
  thread_slice
WHERE
  step IS NOT NULL
  AND latency_id != -1
ORDER BY slice_id, ts;

-- Each row represents one input pipeline.
CREATE PERFETTO TABLE chrome_inputs(
  -- Id of this Chrome input pipeline (LatencyInfo).
  latency_id LONG,
   -- Input type.
  input_type STRING
) AS
SELECT
  -- Id of this Chrome input pipeline (LatencyInfo).
  latency_id,
  -- MIN selects the first non-null value.
  MIN(input_type) as input_type
FROM _chrome_input_pipeline_steps_no_input_type
WHERE latency_id != -1
GROUP BY latency_id;

-- Since not all steps have associated input type (but all steps
-- for a given latency id should have the same input type),
-- populate input type for steps where it would be NULL.
CREATE PERFETTO TABLE chrome_input_pipeline_steps(
  -- Id of this Chrome input pipeline (LatencyInfo).
  latency_id LONG,
  -- Slice id
  slice_id LONG,
  -- The step timestamp.
  ts TIMESTAMP,
  -- Step duration.
  dur DURATION,
  -- Utid of the thread.
  utid LONG,
  -- Step name (ChromeLatencyInfo.step).
  step STRING,
  -- Input type.
  input_type STRING,
  -- Start time of the parent Chrome scheduler task (if any) of this step.
  task_start_time_ts TIMESTAMP
) AS
SELECT
  latency_id,
  slice_id,
  ts,
  dur,
  utid,
  step,
  chrome_inputs.input_type AS input_type,
  task_start_time_ts
FROM
  chrome_inputs
LEFT JOIN
  _chrome_input_pipeline_steps_no_input_type
  USING (latency_id)
WHERE chrome_inputs.input_type IS NOT NULL;

-- For each input, get the latency id of the input that it was coalesced into.
CREATE PERFETTO TABLE chrome_coalesced_inputs(
  -- The `latency_id` of the coalesced input.
  coalesced_latency_id LONG,
  -- The `latency_id` of the input that the current input was coalesced into.
  presented_latency_id LONG
) AS
SELECT
  args.int_value AS coalesced_latency_id,
  latency_id AS presented_latency_id
FROM chrome_input_pipeline_steps step
JOIN slice USING (slice_id)
JOIN args USING (arg_set_id)
WHERE step.step = 'STEP_RESAMPLE_SCROLL_EVENTS'
  AND args.flat_key = 'chrome_latency_info.coalesced_trace_ids';


-- Each scroll update event (except flings) in Chrome starts its life as a touch
-- move event, which is then eventually converted to a scroll update itself.
-- Each of these events is represented by its own LatencyInfo. This table
-- contains a mapping between touch move events and scroll update events they
-- were converted into.
CREATE PERFETTO TABLE chrome_touch_move_to_scroll_update(
  -- Latency id of the touch move input (LatencyInfo).
  touch_move_latency_id LONG,
  -- Latency id of the corresponding scroll update input (LatencyInfo).
  scroll_update_latency_id LONG
) AS
WITH
scroll_update_steps AS (
  SELECT *
  FROM chrome_input_pipeline_steps
  WHERE step = 'STEP_SEND_INPUT_EVENT_UI'
  AND input_type = 'GESTURE_SCROLL_UPDATE_EVENT'
),
-- By default, we map a scroll update event to an ancestor touch move event with
-- STEP_TOUCH_EVENT_HANDLED.
default_mapping AS (
  SELECT
    touch_move_step.latency_id AS touch_move_latency_id,
    scroll_update_steps.latency_id AS scroll_update_latency_id
  FROM scroll_update_steps
  JOIN ancestor_slice(scroll_update_steps.slice_id) AS ancestor
  JOIN chrome_input_pipeline_steps AS touch_move_step
    ON ancestor.id = touch_move_step.slice_id
  WHERE touch_move_step.step = 'STEP_TOUCH_EVENT_HANDLED'
),
-- In the rare case where there are no touch handlers in the renderer, there's
-- no ancestor touch move event with STEP_TOUCH_EVENT_HANDLED. In that case, we
-- try to fall back to an ancestor touch move event with
-- STEP_SEND_INPUT_EVENT_UI instead.
fallback_mapping AS (
  SELECT
    touch_move_step.latency_id AS touch_move_latency_id,
    scroll_update_steps.latency_id AS scroll_update_latency_id
  FROM scroll_update_steps
  JOIN ancestor_slice(scroll_update_steps.slice_id) AS ancestor
  JOIN chrome_input_pipeline_steps AS touch_move_step
    ON ancestor.id = touch_move_step.slice_id
  WHERE touch_move_step.step = 'STEP_SEND_INPUT_EVENT_UI'
  AND touch_move_step.input_type = 'TOUCH_MOVE_EVENT'
)
SELECT
  COALESCE(
    default_mapping.touch_move_latency_id,
    fallback_mapping.touch_move_latency_id
  ) AS touch_move_latency_id,
  scroll_update_latency_id
FROM default_mapping
FULL OUTER JOIN fallback_mapping USING (scroll_update_latency_id);

-- Matches Android input id to the corresponding touch move event.
CREATE PERFETTO TABLE chrome_dispatch_android_input_event_to_touch_move(
  -- Input id (assigned by the system, used by InputReader and InputDispatcher)
  android_input_id STRING,
  -- Latency id.
  touch_move_latency_id LONG
) AS
SELECT
  chrome_deliver_android_input_event.android_input_id,
  latency_id AS touch_move_latency_id
FROM
  chrome_deliver_android_input_event
LEFT JOIN
  chrome_input_pipeline_steps USING (utid)
WHERE
  chrome_input_pipeline_steps.input_type = 'TOUCH_MOVE_EVENT'
  AND chrome_input_pipeline_steps.step = 'STEP_SEND_INPUT_EVENT_UI'
  AND chrome_deliver_android_input_event.ts <= chrome_input_pipeline_steps.ts
  AND chrome_deliver_android_input_event.ts + chrome_deliver_android_input_event.dur >=
    chrome_input_pipeline_steps.ts + chrome_input_pipeline_steps.dur;
