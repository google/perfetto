-- Copyright 2024 The Chromium Authors
-- Use of this source code is governed by a BSD-style license that can be
-- found in the LICENSE file.

-- Finds the start timestamp for a given slice's descendant with a given name.
-- If there are multiple descendants with a given name, the function will return
-- the first one, so it's most useful when working with a timeline broken down
-- into phases, where each subphase can happen only once.
CREATE PERFETTO FUNCTION _descendant_slice_begin(
  -- Id of the parent slice.
  parent_id LONG,
  -- Name of the child with the desired start TS.
  child_name STRING
)
-- Start timestamp of the child or NULL if it doesn't exist.
RETURNS LONG AS
SELECT s.ts
FROM descendant_slice($parent_id) s
WHERE s.name GLOB $child_name
LIMIT 1;

-- Finds the end timestamp for a given slice's descendant with a given name.
-- If there are multiple descendants with a given name, the function will return
-- the first one, so it's most useful when working with a timeline broken down
-- into phases, where each subphase can happen only once.
CREATE PERFETTO FUNCTION _descendant_slice_end(
  -- Id of the parent slice.
  parent_id LONG,
  -- Name of the child with the desired end TS.
  child_name STRING
)
-- End timestamp of the child or NULL if it doesn't exist.
RETURNS LONG AS
SELECT
  CASE WHEN s.dur
    IS NOT -1 THEN s.ts + s.dur
    ELSE NULL
  END
FROM descendant_slice($parent_id) s
WHERE s.name GLOB $child_name
LIMIT 1;

-- Checks if slice has a descendant with provided name.
CREATE PERFETTO FUNCTION _has_descendant_slice_with_name(
  -- Id of the slice to check descendants of.
  id LONG,
  -- Name of potential descendant slice.
  descendant_name STRING
)
-- Whether `descendant_name` is a name of an descendant slice.
RETURNS BOOL AS
SELECT EXISTS(
  SELECT 1
  FROM descendant_slice($id)
  WHERE name = $descendant_name
  LIMIT 1
);

-- Returns the presentation timestamp for a given EventLatency slice.
-- This is either the end of
-- SwapEndToPresentationCompositorFrame (if it exists),
-- the end of LatchToPresentation (if it exists),
-- the end of SwapStartToPresentation (if it exists),
-- or the end of LatchToSwapEnd (workaround in older Chrome versions).
CREATE PERFETTO FUNCTION _get_presentation_timestamp(
  -- The slice id which we need the presentation timestamp for.
  id LONG
)
RETURNS LONG AS
SELECT
  COALESCE(_descendant_slice_end(id, 'SwapEndToPresentationCompositorFrame'),
    _descendant_slice_end(id, '*ToPresentation'),
    _descendant_slice_end(id, 'LatchToSwapEnd'))
FROM slice WHERE $id = id;

-- All EventLatency slices.
CREATE PERFETTO TABLE chrome_event_latencies(
  -- Slice Id for the EventLatency scroll event.
  id LONG,
  -- Slice name.
  name STRING,
  -- The start timestamp of the scroll.
  ts TIMESTAMP,
  -- The duration of the scroll.
  dur DURATION,
  -- The id of the scroll update event.
  scroll_update_id LONG,
  -- Whether this input event was presented.
  is_presented BOOL,
  -- EventLatency event type.
  event_type STRING,
  -- Perfetto track this slice is found on.
  track_id LONG,
  -- Vsync interval (in milliseconds).
  vsync_interval_ms DOUBLE,
  -- Whether the corresponding frame is janky.
  is_janky_scrolled_frame BOOL,
  -- Timestamp of the BufferAvailableToBufferReady substage.
  buffer_available_timestamp LONG,
  -- Timestamp of the BufferReadyToLatch substage.
  buffer_ready_timestamp LONG,
  -- Timestamp of the LatchToSwapEnd substage (or LatchToPresentation as a
  -- fallback).
  latch_timestamp LONG,
  -- Timestamp of the SwapEndToPresentationCompositorFrame substage.
  swap_end_timestamp LONG,
  -- Frame presentation timestamp aka the timestamp of the
  -- SwapEndToPresentationCompositorFrame substage.
  -- TODO(b/341047059): temporarily use LatchToSwapEnd as a workaround if
  -- SwapEndToPresentationCompositorFrame is missing due to b/247542163.
  presentation_timestamp LONG
) AS
SELECT
  slice.id,
  slice.name,
  slice.ts,
  slice.dur,
  EXTRACT_arg(arg_set_id, 'event_latency.event_latency_id') AS scroll_update_id,
  _has_descendant_slice_with_name(
    slice.id,
    'SubmitCompositorFrameToPresentationCompositorFrame')
    AS is_presented,
  EXTRACT_ARG(arg_set_id, 'event_latency.event_type') AS event_type,
  slice.track_id,
  EXTRACT_ARG(arg_set_id, 'event_latency.vsync_interval_ms')
    AS vsync_interval_ms,
  COALESCE(EXTRACT_ARG(arg_set_id, 'event_latency.is_janky_scrolled_frame'), 0)
    AS is_janky_scrolled_frame,
  _descendant_slice_begin(slice.id, 'BufferAvailableToBufferReady')
    AS buffer_available_timestamp,
  _descendant_slice_begin(slice.id, 'BufferReadyToLatch')
    AS buffer_ready_timestamp,
  COALESCE(
    _descendant_slice_begin(slice.id, 'LatchToSwapEnd'),
    _descendant_slice_begin(slice.id, 'LatchToPresentation')
  ) AS latch_timestamp,
  _descendant_slice_begin(slice.id, 'SwapEndToPresentationCompositorFrame')
    AS swap_end_timestamp,
  _get_presentation_timestamp(slice.id) AS presentation_timestamp
FROM slice
WHERE name = 'EventLatency';

-- All EventLatency slices that are relevant to scrolling, including presented
-- pinches. Materialized to reduce how many times we query slice.
CREATE PERFETTO TABLE _gesture_scroll_events_no_scroll_id
AS
SELECT
  name,
  ts,
  dur,
  id,
  scroll_update_id,
  is_presented,
  _get_presentation_timestamp(chrome_event_latencies.id)
  AS presentation_timestamp,
  event_type,
  track_id
FROM chrome_event_latencies
WHERE (
  event_type GLOB '*GESTURE_SCROLL*'
  -- Pinches are only relevant if the frame was presented.
  OR (event_type GLOB '*GESTURE_PINCH_UPDATE'
    AND _has_descendant_slice_with_name(
      id,
      'SubmitCompositorFrameToPresentationCompositorFrame')
  )
);

-- Extracts scroll id for the EventLatency slice at `ts`.
CREATE PERFETTO FUNCTION chrome_get_most_recent_scroll_begin_id(
  -- Timestamp of the EventLatency slice to get the scroll id for.
  ts TIMESTAMP)
-- The event_latency_id of the EventLatency slice with the type
-- GESTURE_SCROLL_BEGIN that is the closest to `ts`.
RETURNS LONG AS
SELECT scroll_update_id
FROM _gesture_scroll_events_no_scroll_id
WHERE event_type = 'GESTURE_SCROLL_BEGIN'
AND ts<=$ts
ORDER BY ts DESC
LIMIT 1;

-- All scroll-related events (frames) including gesture scroll updates, begins
-- and ends with respective scroll ids and start/end timestamps, regardless of
-- being presented. This includes pinches that were presented. See b/315761896
-- for context on pinches.
CREATE PERFETTO TABLE chrome_gesture_scroll_events(
  -- Slice Id for the EventLatency scroll event.
  id LONG,
  -- Slice name.
  name STRING,
  -- The start timestamp of the scroll.
  ts TIMESTAMP,
  -- The duration of the scroll.
  dur DURATION,
  -- The id of the scroll update event.
  scroll_update_id LONG,
  -- The id of the scroll.
  scroll_id LONG,
  -- Whether this input event was presented.
  is_presented BOOL,
  -- Frame presentation timestamp aka the timestamp of the
  -- SwapEndToPresentationCompositorFrame substage.
  -- TODO(b/341047059): temporarily use LatchToSwapEnd as a workaround if
  -- SwapEndToPresentationCompositorFrame is missing due to b/247542163.
  presentation_timestamp LONG,
  -- EventLatency event type.
  event_type STRING,
  -- Perfetto track this slice is found on.
  track_id LONG
) AS
SELECT
  id,
  name,
  ts,
  dur,
  scroll_update_id,
  chrome_get_most_recent_scroll_begin_id(ts) AS scroll_id,
  is_presented,
  presentation_timestamp,
  event_type,
  track_id
FROM _gesture_scroll_events_no_scroll_id;
