-- Copyright 2024 The Chromium Authors
-- Use of this source code is governed by a BSD-style license that can be
-- found in the LICENSE file.

INCLUDE PERFETTO MODULE chrome.android_input;

INCLUDE PERFETTO MODULE slices.with_context;

-- Processing steps of the Chrome input pipeline.
CREATE PERFETTO PIPELINE _chrome_input_pipeline_steps_no_input_type (
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
) MATERIALIZED AS
FROM thread_slice
|> SELECT
  extract_arg(thread_slice.arg_set_id, 'chrome_latency_info.trace_id') AS latency_id,
  id AS slice_id,
  ts,
  dur,
  utid,
  extract_arg(thread_slice.arg_set_id, 'chrome_latency_info.step') AS step,
  extract_arg(thread_slice.arg_set_id, 'chrome_latency_info.input_type') AS input_type,
  ts - (
    extract_arg(thread_slice.arg_set_id, 'current_task.event_offset_from_task_start_time_us') * 1000
  ) AS task_start_time_ts
|> WHERE NOT step IS NULL AND latency_id != -1
-- Partition the steps so that, if the same step (for the same input) was
-- emitted more than once (e.g. due to b:390406106), the step ends up in the
-- same partition as all its duplicates. This enables us to deduplicate the
-- steps later. If there are multiple STEP_RESAMPLE_SCROLL_EVENTS steps, we
-- assume the input was only dispatched after the last resampling, so we only
-- care about the last STEP_RESAMPLE_SCROLL_EVENTS step. We don't have any
-- preference for other steps but, for determinism and consistency, let's
-- always pick the last step.
|> EXTEND row_number() OVER (PARTITION BY latency_id, utid, step, input_type ORDER BY ts DESC) AS ordering_within_partition
-- This is where we actually remove duplicate steps.
|> WHERE ordering_within_partition = 1
|> SELECT latency_id, slice_id, ts, dur, utid, step, input_type, task_start_time_ts
|> ORDER BY slice_id, ts;

-- Each row represents one input pipeline.
CREATE PERFETTO PIPELINE chrome_inputs (
  -- Id of this Chrome input pipeline (LatencyInfo).
  latency_id LONG,
  -- Input type.
  input_type STRING
) MATERIALIZED AS
FROM _chrome_input_pipeline_steps_no_input_type
|> WHERE latency_id != -1
-- MIN selects the first non-null value.
|> AGGREGATE min(input_type) AS input_type GROUP BY latency_id;

-- Since not all steps have associated input type (but all steps
-- for a given latency id should have the same input type),
-- populate input type for steps where it would be NULL.
CREATE PERFETTO PIPELINE chrome_input_pipeline_steps (
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
) MATERIALIZED AS
FROM chrome_inputs
|> LEFT JOIN _chrome_input_pipeline_steps_no_input_type USING (latency_id)
|> WHERE chrome_inputs.input_type IS NOT NULL
|> SELECT
  latency_id,
  slice_id,
  ts,
  dur,
  utid,
  step,
  chrome_inputs.input_type AS input_type,
  task_start_time_ts;

-- For each input, if it was coalesced into another input, get the other input's
-- latency id.
CREATE PERFETTO PIPELINE chrome_coalesced_inputs (
  -- The `latency_id` of the coalesced input.
  coalesced_latency_id LONG,
  -- The `latency_id` of the other input that the current input was coalesced
  -- into. Guaranteed to be different from `coalesced_latency_id`.
  presented_latency_id LONG
) MATERIALIZED AS
FROM chrome_input_pipeline_steps AS step
|> JOIN slice USING (slice_id)
|> JOIN args USING (arg_set_id)
|> WHERE
  step.step = 'STEP_RESAMPLE_SCROLL_EVENTS'
  AND args.flat_key = 'chrome_latency_info.coalesced_trace_ids'
  AND args.int_value != step.latency_id
|> SELECT
  args.int_value AS coalesced_latency_id,
  step.latency_id AS presented_latency_id;

-- Each scroll update event (except flings) in Chrome starts its life as a touch
-- move event, which is then eventually converted to a scroll update itself.
-- Each of these events is represented by its own LatencyInfo. This table
-- contains a mapping between touch move events and scroll update events they
-- were converted into.
CREATE PERFETTO PIPELINE chrome_touch_move_to_scroll_update (
  -- Latency id of the touch move input (LatencyInfo).
  touch_move_latency_id LONG,
  -- Latency id of the corresponding scroll update input (LatencyInfo).
  scroll_update_latency_id LONG
) MATERIALIZED AS
SUBPIPELINE scroll_update_steps AS (
  FROM chrome_input_pipeline_steps
  |> WHERE
    step = 'STEP_SEND_INPUT_EVENT_UI' AND input_type = 'GESTURE_SCROLL_UPDATE_EVENT'
    AND dur > 0
)
SUBPIPELINE touch_handled_steps AS (
  FROM chrome_input_pipeline_steps AS step
  |> WHERE step = 'STEP_TOUCH_EVENT_HANDLED' AND dur > 0
)
SUBPIPELINE send_touch_move_steps AS (
  FROM chrome_input_pipeline_steps AS step
  |> WHERE
    step = 'STEP_SEND_INPUT_EVENT_UI' AND input_type = 'TOUCH_MOVE_EVENT'
    AND dur > 0
)
-- By default, we map a scroll update event to an ancestor touch move event with
-- STEP_TOUCH_EVENT_HANDLED: for each scroll_update_step, find the
-- touch_move_handled step whose bounds fully cover it (i.e. it is an ancestor of
-- the scroll_update_step).
SUBPIPELINE default_mapping AS (
  FROM scroll_update_steps AS su
  |> INTERVAL JOIN touch_handled_steps AS tm COVERING BOUNDS PER utid
  |> SELECT
    tm.latency_id AS touch_move_latency_id,
    su.latency_id AS scroll_update_latency_id
)
-- In the rare case where there are no touch handlers in the renderer, there's
-- no ancestor touch move event with STEP_TOUCH_EVENT_HANDLED. In that case, we
-- try to fall back to an ancestor touch move event with STEP_SEND_INPUT_EVENT_UI
-- instead.
SUBPIPELINE fallback_mapping AS (
  FROM scroll_update_steps AS su
  |> INTERVAL JOIN send_touch_move_steps AS tm COVERING BOUNDS PER utid
  |> SELECT
    tm.latency_id AS touch_move_latency_id,
    su.latency_id AS scroll_update_latency_id
)
-- We ideally would want to do a FULL JOIN here, but it is very slow in SQLite,
-- so instead we are doing UNION + two LEFT JOINs.
FROM default_mapping
|> SELECT scroll_update_latency_id
|> UNION (FROM fallback_mapping |> SELECT scroll_update_latency_id)
|> LEFT JOIN default_mapping USING (scroll_update_latency_id)
|> LEFT JOIN fallback_mapping USING (scroll_update_latency_id)
|> SELECT
  coalesce(default_mapping.touch_move_latency_id, fallback_mapping.touch_move_latency_id) AS touch_move_latency_id,
  scroll_update_latency_id;

-- Matches Android input id to the corresponding touch move event.
CREATE PERFETTO PIPELINE chrome_dispatch_android_input_event_to_touch_move (
  -- Input id (assigned by the system, used by InputReader and InputDispatcher)
  android_input_id STRING,
  -- Latency id.
  touch_move_latency_id LONG
) MATERIALIZED AS
FROM chrome_deliver_android_input_event
|> LEFT JOIN chrome_input_pipeline_steps USING (utid)
|> WHERE
  chrome_input_pipeline_steps.input_type = 'TOUCH_MOVE_EVENT'
  AND chrome_input_pipeline_steps.step = 'STEP_SEND_INPUT_EVENT_UI'
  AND chrome_deliver_android_input_event.ts <= chrome_input_pipeline_steps.ts
  AND chrome_deliver_android_input_event.ts + chrome_deliver_android_input_event.dur >= chrome_input_pipeline_steps.ts + chrome_input_pipeline_steps.dur
|> SELECT
  chrome_deliver_android_input_event.android_input_id,
  chrome_input_pipeline_steps.latency_id AS touch_move_latency_id;
