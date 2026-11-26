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

-- Links input events to the display video: the video frame that was on screen
-- when the event happened, and the video frame that showed the app's response.

INCLUDE PERFETTO MODULE android.input;

INCLUDE PERFETTO MODULE android.video_frames;

-- Displayable video frames, numbered in presentation order per display, so
-- "last frame before" / "first frame after" lookups can be done with window
-- functions over ordered rows instead of correlated subqueries.
CREATE PERFETTO TABLE _android_input_video_frame AS
SELECT
  id,
  display_id,
  ts,
  frame_timeline_vsync_id,
  row_number() OVER (PARTITION BY display_id ORDER BY ts, id) AS seq
FROM android_video_frames;

CREATE PERFETTO INDEX _android_input_video_frame_seq_idx ON _android_input_video_frame(
  display_id,
  seq
);

CREATE PERFETTO INDEX _android_input_video_frame_vsync_idx ON _android_input_video_frame(
  display_id,
  frame_timeline_vsync_id
);

-- The motion events we link: touches (pointer-class sources, `source & 0x2`)
-- going DOWN (0), UP (1), POINTER_DOWN (5) or POINTER_UP (6), as received
-- by InputDispatcher. Copies InputDispatcher derived from them (splits, ...)
-- or generated on its own (cancellations, ...) are left out: they are not
-- touches of their own.
CREATE PERFETTO TABLE _android_input_video_motion AS
SELECT
  e.id AS motion_event_id,
  e.event_id,
  e.event_ts,
  e.display_id,
  e.action & 0xff AS action_masked,
  g.group_id
FROM __intrinsic_android_motion_events AS e
JOIN _android_motion_event_group AS g ON g.motion_event_id = e.id
WHERE
  (e.action & 0xff) IN (0, 1, 5, 6)
  AND (e.source & 0x2) != 0
  AND NOT g.is_copy;

-- The copies InputDispatcher derived from each touch: the copies in the same
-- group (same device and event time) whose pointers are all pointers of the
-- touch. In a group with several touches (e.g. two fingers going down in the
-- same input frame: DOWN, then POINTER_DOWN), this picks the touch a split
-- copy came from. They are dispatched under their own event id. CANCEL (3)
-- and OUTSIDE (4) copies tell other windows about a touch; they aren't
-- handling it.
CREATE PERFETTO TABLE _android_input_video_copy AS
WITH
  pairs AS (
    SELECT
      m.motion_event_id,
      c.motion_event_id AS copy_id,
      c.event_id AS copy_event_id
    FROM _android_input_video_motion AS m
    JOIN _android_motion_event_group AS c ON c.group_id = m.group_id
    WHERE
      c.is_copy
      AND NOT (c.action_masked IN (3, 4))
  )
SELECT pr.motion_event_id, pr.copy_event_id
FROM pairs AS pr
JOIN __intrinsic_android_motion_event_pointer AS cp
  ON cp.motion_event_id = pr.copy_id
LEFT JOIN __intrinsic_android_motion_event_pointer AS ep
  ON ep.motion_event_id = pr.motion_event_id
  AND ep.pointer_id = cp.pointer_id
GROUP BY
  pr.motion_event_id,
  pr.copy_id
HAVING
  count(ep.pointer_id) = count(*);

-- The ids each touch is dispatched under: its own and those of its copies, as
-- lowercase hex like in the atrace input slices.
CREATE PERFETTO TABLE _android_input_video_event_id AS
SELECT
  motion_event_id,
  printf('0x%x', event_id) AS input_event_id,
  0 AS is_derived
FROM _android_input_video_motion
UNION ALL
SELECT
  motion_event_id,
  printf('0x%x', copy_event_id) AS input_event_id,
  1 AS is_derived
FROM _android_input_video_copy;

-- The video frame on screen when each event happened: the last frame of the
-- event's display presented at or before the event time.
CREATE PERFETTO TABLE _android_input_video_visible_frame AS
WITH
  points AS (
    -- At equal timestamps, frames sort before events: a frame presented at
    -- the event time is on screen.
    SELECT display_id, ts, 0 AS kind, seq, NULL AS motion_event_id
    FROM _android_input_video_frame
    UNION ALL
    SELECT display_id, event_ts AS ts, 1 AS kind, NULL AS seq, motion_event_id
    FROM _android_input_video_motion
    WHERE
      event_ts IS NOT NULL
      AND display_id IS NOT NULL
  ),
  running AS (
    SELECT
      display_id,
      kind,
      motion_event_id,
      max(seq) OVER (
        PARTITION BY
          display_id
        ORDER BY ts, kind
        ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW
      ) AS visible_seq
    FROM points
  )
SELECT r.motion_event_id, f.id AS visible_video_frame_id
FROM running AS r
JOIN _android_input_video_frame AS f
  ON f.display_id = r.display_id
  AND f.seq = r.visible_seq
WHERE
  r.kind = 1;

CREATE PERFETTO INDEX _android_input_video_visible_frame_idx ON _android_input_video_visible_frame(
  motion_event_id
);

-- The atrace-based handling of each input event, one row per receiving
-- channel, keyed by the event id.
CREATE PERFETTO TABLE _android_input_video_handling AS
SELECT
  input_event_id,
  upid,
  process_name,
  event_channel,
  dispatch_ts,
  dispatch_slice_id,
  receive_ts,
  receive_slice_id,
  frame_id AS app_vsync_id,
  handling_latency_dur
FROM android_input_events
WHERE
  input_event_id IS NOT NULL;

CREATE PERFETTO INDEX _android_input_video_handling_idx ON _android_input_video_handling(
  input_event_id
);

-- The handling of each motion event per receiving channel, under its own id
-- or the id of an event derived from it. Exactly one row per (motion event,
-- channel): the event's own id first, then the earliest dispatch.
CREATE PERFETTO TABLE _android_input_video_channel AS
SELECT
  motion_event_id,
  upid,
  process_name,
  event_channel,
  dispatch_ts,
  dispatch_slice_id,
  receive_ts,
  receive_slice_id,
  app_vsync_id,
  handling_latency_dur
FROM (
  SELECT
    i.motion_event_id,
    h.*,
    row_number() OVER (
      PARTITION BY
        i.motion_event_id,
        h.event_channel
      ORDER BY i.is_derived, h.dispatch_ts
    ) AS rn
  FROM _android_input_video_event_id AS i
  JOIN _android_input_video_handling AS h USING (input_event_id)
)
WHERE
  rn = 1;

CREATE PERFETTO INDEX _android_input_video_channel_idx ON _android_input_video_channel(
  motion_event_id
);

-- The SurfaceFlinger DisplayFrame that presented each app frame: the app's
-- actual frame timeline slice for that vsync (surface frame token). An app
-- frame with several layers can appear more than once: take the earliest
-- DisplayFrame. Dropped app frames are never presented, so they don't count.
CREATE PERFETTO TABLE _android_input_video_app_frame AS
SELECT
  upid,
  surface_frame_token AS app_vsync_id,
  min(display_frame_token) AS display_frame_token
FROM actual_frame_timeline_slice
WHERE
  surface_frame_token IS NOT NULL
  AND display_frame_token IS NOT NULL
  AND upid IS NOT NULL
  AND coalesce(present_type, '') != 'Dropped Frame'
GROUP BY
  upid,
  surface_frame_token;

CREATE PERFETTO INDEX _android_input_video_app_frame_idx ON _android_input_video_app_frame(
  upid,
  app_vsync_id
);

-- When each SurfaceFlinger DisplayFrame was presented: the end of its actual
-- frame timeline slice (the earliest, if there are several displays).
CREATE PERFETTO TABLE _android_input_video_display_frame AS
SELECT display_frame_token, min(ts + dur) AS present_ts
FROM actual_frame_timeline_slice
WHERE
  surface_frame_token IS NULL
  AND display_frame_token IS NOT NULL
  AND dur >= 0
GROUP BY
  display_frame_token;

CREATE PERFETTO INDEX _android_input_video_display_frame_idx ON _android_input_video_display_frame(
  display_frame_token
);

-- One row per (motion event, receiving channel); a motion event that no
-- channel can be matched to (e.g. without atrace) gets one row with NULL
-- handling columns.
CREATE PERFETTO TABLE _android_input_video_row AS
SELECT
  row_number() OVER (ORDER BY m.motion_event_id, h.event_channel) AS row_id,
  m.motion_event_id,
  m.event_id,
  m.event_ts,
  m.display_id,
  m.action_masked,
  h.upid,
  h.process_name,
  h.event_channel,
  h.dispatch_ts,
  h.dispatch_slice_id,
  h.receive_ts,
  h.receive_slice_id,
  h.app_vsync_id,
  h.handling_latency_dur,
  af.display_frame_token,
  df.present_ts AS display_frame_present_ts
FROM _android_input_video_motion AS m
LEFT JOIN _android_input_video_channel AS h USING (motion_event_id)
LEFT JOIN _android_input_video_app_frame AS af
  ON af.upid = h.upid
  AND af.app_vsync_id = h.app_vsync_id
LEFT JOIN _android_input_video_display_frame AS df USING (display_frame_token);

-- The video frame showing each response. Exact: the frame produced by the
-- response's DisplayFrame (same vsync id). Otherwise the earliest of: the
-- next frame by vsync id (from a later composite) and the next frame by
-- present time (presented at or after the DisplayFrame's present time).
CREATE PERFETTO TABLE _android_input_video_response_frame AS
WITH
  exact AS (
    SELECT r.row_id, min(f.seq) AS seq
    FROM _android_input_video_row AS r
    JOIN _android_input_video_frame AS f
      ON f.display_id = r.display_id
      AND f.frame_timeline_vsync_id = r.display_frame_token
    GROUP BY
      r.row_id
  ),
  -- At equal keys, rows sort before frames so that the frame counts as
  -- "at or after" the row.
  by_vsync AS (
    SELECT
      row_id,
      kind,
      min(seq) OVER (
        PARTITION BY
          display_id
        ORDER BY vsync, kind
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
      ) AS seq
    FROM (
      SELECT
        row_id,
        display_id,
        display_frame_token AS vsync,
        0 AS kind,
        NULL AS seq
      FROM _android_input_video_row
      WHERE
        display_frame_token IS NOT NULL
        AND display_id IS NOT NULL
      UNION ALL
      SELECT
        NULL AS row_id,
        display_id,
        frame_timeline_vsync_id AS vsync,
        1 AS kind,
        seq
      FROM _android_input_video_frame
      WHERE
        frame_timeline_vsync_id IS NOT NULL
    )
  ),
  by_time AS (
    SELECT
      row_id,
      kind,
      min(seq) OVER (
        PARTITION BY
          display_id
        ORDER BY ts, kind
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
      ) AS seq
    FROM (
      SELECT
        row_id,
        display_id,
        display_frame_present_ts AS ts,
        0 AS kind,
        NULL AS seq
      FROM _android_input_video_row
      WHERE
        display_frame_present_ts IS NOT NULL
        AND display_id IS NOT NULL
      UNION ALL
      SELECT NULL AS row_id, display_id, ts, 1 AS kind, seq
      FROM _android_input_video_frame
    )
  ),
  resolved AS (
    SELECT
      r.row_id,
      r.display_id,
      e.seq AS exact_seq,
      CASE
        WHEN v.seq IS NULL THEN t.seq
        WHEN t.seq IS NULL THEN v.seq
        ELSE min(v.seq, t.seq)
      END AS fallback_seq
    FROM _android_input_video_row AS r
    LEFT JOIN exact AS e USING (row_id)
    LEFT JOIN by_vsync AS v ON v.row_id = r.row_id AND v.kind = 0
    LEFT JOIN by_time AS t ON t.row_id = r.row_id AND t.kind = 0
  )
SELECT
  r.row_id,
  f.id AS response_video_frame_id,
  f.ts AS response_ts,
  r.exact_seq IS NOT NULL AS is_response_exact
FROM resolved AS r
JOIN _android_input_video_frame AS f
  ON f.display_id = r.display_id
  AND f.seq = coalesce(r.exact_seq, r.fallback_seq);

CREATE PERFETTO INDEX _android_input_video_response_frame_idx ON _android_input_video_response_frame(
  row_id
);

-- Input events linked to the display video (android.display.video): for each
-- touch DOWN/UP/POINTER_DOWN/POINTER_UP, the video frame that was on screen
-- when it happened and the video frame that showed the app's response.
--
-- Only touches are included: events from pointer-class sources
-- (`source & 0x2`, e.g. touchscreen, mouse, stylus) received by
-- InputDispatcher. Copies of a touch that InputDispatcher derives for other
-- windows (e.g. the halves of a split touch) and events it generates on its
-- own (e.g. cancellations) are left out, but the dispatches of a touch's
-- copies are attributed to the touch: a split touch is handled under the
-- copy's id, so its channels show up on the touch (CANCEL and OUTSIDE copies
-- are not attributed). Copies are recognized by their event id, which
-- InputDispatcher generates, and linked to their touch by device and event
-- time.
--
-- There is one row per (motion_event_id, event_channel): an event delivered
-- to several windows (e.g. an app plus gesture monitors) gets one row per
-- receiving channel, each with its own response. A motion event that can't be
-- matched to any channel (e.g. no atrace `input` category) gets a single row
-- with NULL handling columns.
--
-- The response is found by following the event to the app frame that handled
-- it (from atrace, see `android_input_events.frame_id`), to the SurfaceFlinger
-- DisplayFrame that presented that frame (frame timeline), to the video frame
-- produced by that DisplayFrame's composite (same vsync id). If no video frame
-- has that vsync id, the earliest of the next frame by vsync id and the next
-- frame by present time is used (`is_response_exact` = 0). The response
-- columns are NULL when any link is missing: no atrace, no frame timeline, a
-- dropped app frame, or no video.
CREATE PERFETTO TABLE android_input_event_video_frames(
  -- Id of the motion event.
  motion_event_id JOINID(android_motion_events.id),
  -- The randomly-generated ID of the input event.
  event_id LONG,
  -- The time the event happened (generated by the kernel), in trace time.
  event_ts TIMESTAMP,
  -- Logical id of the display the event is on; also the display of the
  -- video frames.
  display_id LONG,
  -- MotionEvent action without the pointer index: 0 = DOWN, 1 = UP,
  -- 5 = POINTER_DOWN, 6 = POINTER_UP.
  action_masked LONG,
  -- The video frame on screen when the event happened: the last frame of the
  -- display presented at or before `event_ts`. NULL if there is none.
  visible_video_frame_id JOINID(android_video_frames.id),
  -- Upid of the process receiving the event on this channel.
  upid JOINID(process.id),
  -- Name of the process receiving the event on this channel.
  process_name STRING,
  -- Input channel the event was delivered on.
  event_channel STRING,
  -- Timestamp the event was dispatched on this channel.
  dispatch_ts TIMESTAMP,
  -- The InputDispatcher sendMessage slice of the dispatch.
  dispatch_slice_id JOINID(slice.id),
  -- Timestamp the event was received by the app.
  receive_ts TIMESTAMP,
  -- The app's receiveMessage slice.
  receive_slice_id JOINID(slice.id),
  -- Vsync id of the app frame (Choreographer#doFrame) that handled the event.
  app_vsync_id LONG,
  -- Duration from the event being received to the app ACKing it.
  handling_latency_dur DURATION,
  -- Token of the SurfaceFlinger DisplayFrame that presented the app frame
  -- (`actual_frame_timeline_slice.display_frame_token`).
  display_frame_token LONG,
  -- The video frame showing the response: the frame with
  -- `frame_timeline_vsync_id` = `display_frame_token`, else the earliest of
  -- the next frame by vsync id and the next frame by present time.
  response_video_frame_id JOINID(android_video_frames.id),
  -- Whether the response frame is the exact vsync id match.
  is_response_exact BOOL,
  -- Duration from `event_ts` to the response video frame's `ts`: the
  -- presentation time of the virtual-display composite captured into the
  -- video. That is when the response was composited for the screen, which
  -- can be up to about a vsync before the panel actually showed it.
  input_to_photon_dur DURATION
)
AS
SELECT
  r.motion_event_id,
  r.event_id,
  r.event_ts,
  r.display_id,
  r.action_masked,
  v.visible_video_frame_id,
  r.upid,
  r.process_name,
  r.event_channel,
  r.dispatch_ts,
  r.dispatch_slice_id,
  r.receive_ts,
  r.receive_slice_id,
  r.app_vsync_id,
  r.handling_latency_dur,
  r.display_frame_token,
  rf.response_video_frame_id,
  rf.is_response_exact,
  rf.response_ts - r.event_ts AS input_to_photon_dur
FROM _android_input_video_row AS r
LEFT JOIN _android_input_video_visible_frame AS v USING (motion_event_id)
LEFT JOIN _android_input_video_response_frame AS rf USING (row_id)
ORDER BY
  r.row_id;
