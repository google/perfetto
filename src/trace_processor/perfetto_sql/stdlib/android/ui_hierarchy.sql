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

-- UI hierarchy (Views + Jetpack Compose) captured by the android.ui.hierarchy
-- data source.

-- One row per captured snapshot packet (keyframe or delta).
CREATE PERFETTO VIEW android_ui_hierarchy_snapshot(
  -- Row id.
  id LONG,
  -- Capture timestamp.
  ts TIMESTAMP,
  -- Process that emitted the snapshot.
  upid JOINID(process.id),
  -- Whether the snapshot carries the full tree.
  is_keyframe BOOL,
  -- Choreographer vsync id of the captured frame, if known (joins
  -- android_frames.frame_id).
  frame_id LONG,
  -- Applied text mode (1=REDACT_SENSITIVE, 2=REDACT_ALL, 3=INCLUDE_ALL).
  text_mode LONG,
  -- Number of nodes carried by the packet.
  changed_node_count LONG,
  -- Number of nodes removed by the packet.
  removed_node_count LONG
)
AS
SELECT
  id,
  ts,
  upid,
  is_keyframe,
  vsync_id AS frame_id,
  text_mode,
  changed_node_count,
  removed_node_count
FROM __intrinsic_ui_hierarchy_snapshot;

-- App windows. One row per distinct version of a window.
CREATE PERFETTO VIEW android_ui_hierarchy_window(
  -- Row id.
  id LONG,
  -- Start of the interval in which this version was live.
  ts TIMESTAMP,
  -- Duration of the interval (clamped to the trace end).
  dur DURATION,
  -- Owning process.
  upid JOINID(process.id),
  -- Producer-assigned window id.
  window_id LONG,
  -- Window title (as in android_viewcapture.window_name).
  window_name STRING,
  -- Logical display id.
  display_id LONG,
  -- Bounds in screen px.
  bounds_left LONG,
  -- Bounds in screen px.
  bounds_top LONG,
  -- Bounds in screen px.
  bounds_right LONG,
  -- Bounds in screen px.
  bounds_bottom LONG,
  -- Whether the window has input focus.
  has_focus BOOL,
  -- Bitmask of PendingWork (0 = settled).
  pending_work_mask LONG
)
AS
SELECT
  id,
  ts,
  iif(dur = -1, trace_end() - ts, dur) AS dur,
  upid,
  window_id,
  title AS window_name,
  display_id,
  bounds_left,
  bounds_top,
  bounds_right,
  bounds_bottom,
  has_focus,
  pending_work_mask
FROM __intrinsic_ui_hierarchy_window;

-- UI nodes (Views, Compose semantics nodes, composables). One row per distinct
-- version of a node: a new row starts whenever any property changes, so this
-- is the complete history of the UI tree.
CREATE PERFETTO VIEW android_ui_hierarchy_node(
  -- Row id.
  id LONG,
  -- Start of the interval in which this version was live.
  ts TIMESTAMP,
  -- Duration of the interval (clamped to the trace end).
  dur DURATION,
  -- Owning process.
  upid JOINID(process.id),
  -- Window the node belongs to.
  window_id LONG,
  -- Stable node id.
  node_id LONG,
  -- Parent node id (NULL for window roots).
  parent_id LONG,
  -- Index among siblings.
  child_index LONG,
  -- Node kind: 'view', 'compose_view', 'compose_node', 'compose_layout' or 'composable'.
  kind STRING,
  -- View class or composable name.
  name STRING,
  -- Source location, if known.
  source_location STRING,
  -- Bounds in screen px.
  bounds_left LONG,
  -- Bounds in screen px.
  bounds_top LONG,
  -- Bounds in screen px.
  bounds_right LONG,
  -- Bounds in screen px.
  bounds_bottom LONG,
  -- Recorded offset in the parent (px).
  local_x DOUBLE,
  -- Recorded offset in the parent (px).
  local_y DOUBLE,
  -- Recorded size (px), before transforms.
  width LONG,
  -- Recorded size (px), before transforms.
  height LONG,
  -- Recorded local->parent 3x3 matrix (row-major) when not a pure translation.
  transform STRING,
  -- Lookahead (target) offset in the parent.
  lookahead_x LONG,
  -- Lookahead (target) offset in the parent.
  lookahead_y LONG,
  -- Lookahead (target) size.
  lookahead_width LONG,
  -- Lookahead (target) size.
  lookahead_height LONG,
  -- Effective alpha.
  alpha DOUBLE,
  -- Whether this node and all its ancestors are visible.
  is_effectively_visible BOOL,
  -- Alpha multiplied by the effective alpha of its parent.
  effective_alpha DOUBLE,
  -- UiNode.Flag bitmask.
  flags LONG,
  -- Whether the node is visible (FLAG_VISIBLE).
  is_visible BOOL,
  -- Whether the node is clickable (FLAG_CLICKABLE).
  is_clickable BOOL,
  -- Whether the node is focused (FLAG_FOCUSED).
  is_focused BOOL,
  -- Whether the text was redacted (FLAG_TEXT_REDACTED).
  is_text_redacted BOOL,
  -- View.GONE.
  is_gone BOOL,
  -- Text (possibly redacted).
  text STRING,
  -- Accessibility content description.
  content_description STRING,
  -- Compose testTag or View resource entry name.
  test_tag STRING,
  -- Semantic role.
  role STRING,
  -- Accessibility state description.
  state_description STRING,
  -- Comma separated semantic actions.
  actions STRING,
  -- Newline separated name=value properties.
  properties STRING,
  -- Elevation in px.
  elevation DOUBLE,
  -- Scroll offset X px.
  scroll_x LONG,
  -- Scroll offset Y px.
  scroll_y LONG,
  -- System.identityHashCode of the View.
  hashcode LONG,
  -- Drawn and clipped left bound in screen px.
  clip_left LONG,
  -- Drawn and clipped top bound in screen px.
  clip_top LONG,
  -- Drawn and clipped right bound in screen px.
  clip_right LONG,
  -- Drawn and clipped bottom bound in screen px.
  clip_bottom LONG,
  -- Whether the node clips its children (FLAG_CLIPS_CHILDREN).
  is_clips_children BOOL,
  -- Window-global paint order.
  draw_order LONG,
  -- View.getZ() or Compose effective zIndex/elevation.
  z DOUBLE,
  -- Semantic key for anchor nodes.
  semantic_key STRING,
  -- Normalized semantic role.
  semantic_role STRING
)
AS
SELECT
  id,
  ts,
  iif(dur = -1, trace_end() - ts, dur) AS dur,
  upid,
  window_id,
  node_id,
  parent_node_id AS parent_id,
  child_index,
  CASE kind
    WHEN 1 THEN 'view'
    WHEN 2 THEN 'compose_view'
    WHEN 3 THEN 'compose_node'
    WHEN 4 THEN 'composable'
    WHEN 5 THEN 'compose_layout'
    ELSE 'unknown'
  END AS kind,
  name,
  source_location,
  bounds_left,
  bounds_top,
  bounds_right,
  bounds_bottom,
  local_x,
  local_y,
  width,
  height,
  transform,
  lookahead_x,
  lookahead_y,
  lookahead_width,
  lookahead_height,
  alpha,
  is_effectively_visible != 0 AS is_effectively_visible,
  effective_alpha,
  flags,
  (flags & 1) != 0 AS is_visible,
  (flags & 4) != 0 AS is_clickable,
  (flags & 32) != 0 AS is_focused,
  (flags & 131072) != 0 AS is_text_redacted,
  (flags & 524288) != 0 AS is_gone,
  text,
  content_description,
  test_tag,
  role,
  state_description,
  actions,
  properties,
  elevation,
  scroll_x,
  scroll_y,
  hashcode,
  clip_left,
  clip_top,
  clip_right,
  clip_bottom,
  (flags & 65536) != 0 AS is_clips_children,
  draw_order,
  z,
  semantic_key,
  semantic_role
FROM __intrinsic_ui_hierarchy_node;

-- The full UI tree (all windows) live at a given timestamp.
CREATE PERFETTO FUNCTION android_ui_hierarchy_nodes_at(
  -- Timestamp to query.
  ts TIMESTAMP
)
RETURNS TABLE(
  -- android_ui_hierarchy_node row id.
  id LONG,
  -- Owning process.
  upid JOINID(process.id),
  -- Window id.
  window_id LONG,
  -- Node id.
  node_id LONG,
  -- Parent node id.
  parent_id LONG,
  -- Index among siblings.
  child_index LONG,
  -- Node kind.
  kind STRING,
  -- Class / composable name.
  name STRING,
  -- Bounds in screen px.
  bounds_left LONG,
  -- Bounds in screen px.
  bounds_top LONG,
  -- Bounds in screen px.
  bounds_right LONG,
  -- Bounds in screen px.
  bounds_bottom LONG,
  -- Flags.
  flags LONG,
  -- Text.
  text STRING,
  -- Content description.
  content_description STRING,
  -- Test tag.
  test_tag STRING,
  -- Role.
  role STRING
)
AS
SELECT
  id,
  upid,
  window_id,
  node_id,
  parent_id,
  child_index,
  kind,
  name,
  bounds_left,
  bounds_top,
  bounds_right,
  bounds_bottom,
  flags,
  text,
  content_description,
  test_tag,
  role
FROM android_ui_hierarchy_node
WHERE
  ts <= $ts
  AND (ts + dur > $ts OR ts + dur = trace_end());

-- Visible, user-meaningful nodes live at a timestamp, in reading order. A
-- compact "what is on screen" dump, e.g. for LLM agents.
CREATE PERFETTO FUNCTION android_ui_hierarchy_screen_text(
  -- Timestamp to query.
  ts TIMESTAMP
)
RETURNS TABLE(
  -- Owning process.
  upid JOINID(process.id),
  -- Node id.
  node_id LONG,
  -- Role (or class name when no role is set).
  role STRING,
  -- Text or content description.
  label STRING,
  -- Test tag.
  test_tag STRING,
  -- Bounds in screen px.
  bounds_left LONG,
  -- Bounds in screen px.
  bounds_top LONG,
  -- Bounds in screen px.
  bounds_right LONG,
  -- Bounds in screen px.
  bounds_bottom LONG,
  -- Whether the node is clickable.
  is_clickable BOOL
)
AS
SELECT
  upid,
  node_id,
  coalesce(role, name) AS role,
  coalesce(text, content_description) AS label,
  test_tag,
  bounds_left,
  bounds_top,
  bounds_right,
  bounds_bottom,
  (flags & 4) != 0 AS is_clickable
FROM android_ui_hierarchy_nodes_at($ts)
WHERE
  (flags & 1) != 0
  AND bounds_right > bounds_left
  AND bounds_bottom > bounds_top
  AND (text IS NOT NULL OR content_description IS NOT NULL OR (flags & 4) != 0)
ORDER BY
  bounds_top,
  bounds_left;

-- The deepest (smallest area) visible node containing a screen point at a
-- timestamp, e.g. to map an input event to the UI element it hit.
CREATE PERFETTO FUNCTION android_ui_hierarchy_node_at_point(
  -- Timestamp to query.
  ts TIMESTAMP,
  -- X coordinate in screen px.
  x LONG,
  -- Y coordinate in screen px.
  y LONG
)
RETURNS TABLE(
  -- android_ui_hierarchy_node row id.
  id LONG,
  -- Node id.
  node_id LONG,
  -- Node kind.
  kind STRING,
  -- Class / composable name.
  name STRING,
  -- Text.
  text STRING,
  -- Test tag.
  test_tag STRING,
  -- Role.
  role STRING
)
AS
SELECT id, node_id, kind, name, text, test_tag, role
FROM android_ui_hierarchy_nodes_at($ts)
WHERE
  (flags & 1) != 0
  AND bounds_left <= $x
  AND $x < bounds_right
  AND bounds_top <= $y
  AND $y < bounds_bottom
ORDER BY
  (bounds_right - bounds_left) * (bounds_bottom - bounds_top) ASC
LIMIT 1;

-- Compose runtime events (not sampled): composition passes, recompose scopes,
-- invalidations with their causing state, disposals, state changes, state
-- reads and composable calls.
CREATE PERFETTO VIEW android_ui_hierarchy_compose_event(
  -- Row id.
  id LONG,
  -- Timestamp (start for slice-like events).
  ts TIMESTAMP,
  -- Duration for slice-like events, else 0.
  dur DURATION,
  -- Owning process.
  upid JOINID(process.id),
  -- Thread.
  utid JOINID(thread.id),
  -- Event type, e.g. 'composition', 'scope', 'scope_invalidated',
  -- 'state_changed', 'composable_call', 'frame', 'node_inserted', 'measure',
  -- 'draw', 'pointer_input', 'animation_frame', 'scroll' (see
  -- UiEvent.Type in ui_hierarchy.proto).
  type STRING,
  -- Composable/scope name ("fn (File.kt:line)") or state type.
  name STRING,
  -- Recompose scope id.
  scope_id LONG,
  -- State object id.
  state_id LONG,
  -- State value description (subject to text_mode).
  value STRING,
  -- Nesting depth.
  depth LONG,
  -- Compose $changed bits (composable calls).
  dirty1 LONG,
  -- Compose $changed bits, second word.
  dirty2 LONG,
  -- Composition id.
  composition_id LONG,
  -- Layout node id (== node id in snapshots).
  node_id LONG,
  -- Parent layout node id.
  parent_id LONG,
  -- Child index / lazy item index.
  node_index LONG,
  -- Source index for moves.
  from_index LONG,
  -- X px (place: in parent; input: in window).
  x DOUBLE,
  -- Y px.
  y DOUBLE,
  -- Width px.
  width LONG,
  -- Height px.
  height LONG,
  -- Measure constraint.
  min_width LONG,
  -- Measure constraint (-1 = unbounded).
  max_width LONG,
  -- Measure constraint.
  min_height LONG,
  -- Measure constraint (-1 = unbounded).
  max_height LONG,
  -- Animation / transition / remember observer id.
  object_id LONG,
  -- Recomposer frame time.
  frame_time_ns LONG,
  -- Animation play time.
  play_time_ns LONG,
  -- Pointer id.
  pointer_id LONG,
  -- Type-specific action / reason code.
  action LONG,
  -- Whether input / scroll was consumed.
  consumed BOOL,
  -- Requested scroll delta.
  delta DOUBLE,
  -- Consumed scroll delta.
  consumed_delta DOUBLE,
  -- Animation spec.
  spec STRING,
  -- Window id.
  window_id LONG,
  -- Platform input event id (MotionEvent/KeyEvent id) for correlation with android_input_events.
  input_event_id LONG,
  -- Whether measure/place was in lookahead pass.
  is_lookahead BOOL,
  -- Semantic key for app state / interaction events.
  semantic_key STRING,
  -- Comma-separated extra args for events.
  args STRING
)
AS
SELECT
  id,
  ts,
  dur,
  upid,
  utid,
  CASE type
    WHEN 1 THEN 'composition'
    WHEN 2 THEN 'scope'
    WHEN 3 THEN 'scope_invalidated'
    WHEN 4 THEN 'scope_disposed'
    WHEN 5 THEN 'state_changed'
    WHEN 6 THEN 'state_read'
    WHEN 7 THEN 'composable_call'
    WHEN 8 THEN 'frame'
    WHEN 9 THEN 'apply_changes'
    WHEN 10 THEN 'remembered'
    WHEN 11 THEN 'forgotten'
    WHEN 12 THEN 'side_effect'
    WHEN 13 THEN 'state_write'
    WHEN 20 THEN 'node_inserted'
    WHEN 21 THEN 'node_removed'
    WHEN 22 THEN 'node_moved'
    WHEN 23 THEN 'node_attached'
    WHEN 24 THEN 'node_detached'
    WHEN 25 THEN 'node_modifier_changed'
    WHEN 26 THEN 'invalidate_measure'
    WHEN 27 THEN 'invalidate_layout'
    WHEN 28 THEN 'invalidate_draw'
    WHEN 29 THEN 'measure'
    WHEN 30 THEN 'place'
    WHEN 31 THEN 'draw'
    WHEN 32 THEN 'measure_and_layout'
    WHEN 33 THEN 'owner_draw'
    WHEN 34 THEN 'semantics_changed'
    WHEN 40 THEN 'pointer_input'
    WHEN 41 THEN 'key_input'
    WHEN 42 THEN 'focus_changed'
    WHEN 50 THEN 'animation_start'
    WHEN 51 THEN 'animation_frame'
    WHEN 52 THEN 'animation_end'
    WHEN 53 THEN 'transition_state'
    WHEN 54 THEN 'transition_frame'
    WHEN 60 THEN 'scroll'
    WHEN 61 THEN 'lazy_item_composed'
    WHEN 62 THEN 'lazy_item_disposed'
    WHEN 63 THEN 'lazy_item_prefetched'
    WHEN 70 THEN 'app_state'
    WHEN 71 THEN 'interaction'
    ELSE 'unknown'
  END AS type,
  name,
  scope_id,
  state_id,
  value,
  depth,
  dirty1,
  dirty2,
  composition_id,
  node_id,
  parent_id,
  node_index,
  from_index,
  x,
  y,
  width,
  height,
  min_width,
  max_width,
  min_height,
  max_height,
  object_id,
  frame_time_ns,
  play_time_ns,
  pointer_id,
  action,
  consumed,
  delta,
  consumed_delta,
  spec,
  window_id,
  input_event_id,
  is_lookahead != 0 AS is_lookahead,
  semantic_key,
  args
FROM __intrinsic_ui_hierarchy_event;

-- Why each recompose scope executed: every scope execution joined with the
-- invalidations (and the state that caused them) recorded since the previous
-- execution of the same scope.
CREATE PERFETTO TABLE android_ui_hierarchy_recomposition_cause(
  -- Scope execution event id.
  scope_event_id LONG,
  -- Scope execution start.
  ts TIMESTAMP,
  -- Scope execution duration.
  dur DURATION,
  -- Owning process.
  upid JOINID(process.id),
  -- Recompose scope id.
  scope_id LONG,
  -- Composable name of the scope.
  name STRING,
  -- Invalidating state object id (NULL for explicit invalidate()).
  state_id LONG,
  -- Invalidating state type.
  state_type STRING,
  -- Invalidating state value at invalidation time.
  state_value STRING
)
AS
WITH
  scopes AS (
    SELECT
      id,
      ts,
      dur,
      upid,
      scope_id,
      name,
      lag(ts) OVER (PARTITION BY upid, scope_id ORDER BY ts) AS prev_ts
    FROM android_ui_hierarchy_compose_event
    WHERE
      type = 'scope'
  ),
  invalidations AS (
    SELECT ts, upid, scope_id, state_id, value
    FROM android_ui_hierarchy_compose_event
    WHERE
      type = 'scope_invalidated'
  ),
  state_types AS (
    SELECT upid, state_id, max(name) AS state_type
    FROM android_ui_hierarchy_compose_event
    WHERE
      type IN ('state_changed', 'state_read')
      AND state_id IS NOT NULL
    GROUP BY
      upid,
      state_id
  )
SELECT
  s.id AS scope_event_id,
  s.ts,
  s.dur,
  s.upid,
  s.scope_id,
  s.name,
  i.state_id,
  st.state_type,
  i.value AS state_value
FROM scopes AS s
JOIN invalidations AS i
  ON i.upid IS s.upid
  AND i.scope_id = s.scope_id
  AND i.ts <= s.ts
  AND i.ts > coalesce(s.prev_ts, 0)
LEFT JOIN state_types AS st
  ON st.upid IS i.upid
  AND st.state_id = i.state_id;

-- Per-window capture completeness: draws, captured frames and skipped
-- (uncaptured) draws. skipped_frames = 0 means no drawn state was missed.
CREATE PERFETTO TABLE android_ui_hierarchy_capture_completeness(
  -- Owning process.
  upid JOINID(process.id),
  -- Window id.
  window_id LONG,
  -- First frame number seen.
  first_frame LONG,
  -- Last frame number seen.
  last_frame LONG,
  -- Window messages emitted (frames with changes, plus keyframes).
  emitted_frames LONG,
  -- Draws that were not captured.
  skipped_frames LONG
)
AS
SELECT
  upid,
  window_id,
  min(frame_number) AS first_frame,
  max(frame_number) AS last_frame,
  count(*) AS emitted_frames,
  sum(skipped_frames) AS skipped_frames
FROM __intrinsic_ui_hierarchy_window_frame
WHERE
  NOT is_removed
GROUP BY
  upid,
  window_id;

-- Maps a screen recording video frame to the UI snapshot closest in time.
CREATE PERFETTO FUNCTION android_ui_hierarchy_snapshot_for_video_frame(
  -- Video frame id (from __intrinsic_video_frames.id).
  video_frame_id LONG
)
RETURNS TABLE(
  -- android_ui_hierarchy_snapshot row id.
  id LONG
)
AS
SELECT snapshot.id
FROM android_ui_hierarchy_snapshot AS snapshot
JOIN __intrinsic_video_frames AS video
  ON video.id = $video_frame_id
ORDER BY
  abs(snapshot.ts - video.ts),
  snapshot.id
LIMIT 1;

-- Maps a UI snapshot to the screen recording video frame closest in time.
CREATE PERFETTO FUNCTION android_video_frame_for_ui_hierarchy_snapshot(
  -- Snapshot id (from android_ui_hierarchy_snapshot.id).
  snapshot_id LONG
)
RETURNS TABLE(
  -- Video frame id (from __intrinsic_video_frames.id).
  id LONG
)
AS
SELECT video.id
FROM __intrinsic_video_frames AS video
JOIN android_ui_hierarchy_snapshot AS snapshot
  ON snapshot.id = $snapshot_id
ORDER BY
  abs(video.ts - snapshot.ts),
  video.id
LIMIT 1;

-- SystemUI state changes (UiStateEvent): one row per value of a field, lasting
-- until the next change of the same field in the same process (or the trace
-- end).
CREATE PERFETTO VIEW android_sysui_state(
  -- Timestamp of the change.
  ts TIMESTAMP,
  -- How long the value held.
  dur DURATION,
  -- Process that reported the change.
  upid JOINID(process.id),
  -- Field name, e.g. 'shade_expansion'.
  field STRING,
  -- Value, for float fields.
  value_float DOUBLE,
  -- Value, for boolean fields.
  value_bool BOOL,
  -- Value, for string fields.
  value_string STRING,
  -- Value, for sets of notification keys or overlay names (comma
  -- separated).
  value_key STRING
)
AS
SELECT
  ts,
  coalesce(
    lead(ts) OVER (PARTITION BY upid, field ORDER BY ts, id),
    trace_end()
  )
  - ts AS dur,
  upid,
  field,
  value_float,
  value_bool,
  value_string,
  value_key
FROM __intrinsic_ui_hierarchy_sysui_state;

-- SystemUI state (UiHierarchySnapshot.sysui_state) at each snapshot that
-- carries it.
CREATE PERFETTO VIEW android_ui_hierarchy_snapshot_state(
  -- android_ui_hierarchy_snapshot row id.
  snapshot_id JOINID(android_ui_hierarchy_snapshot.id),
  -- Shade expansion (0-1).
  shade_expansion DOUBLE,
  -- QS expansion (0-1).
  qs_expansion DOUBLE,
  -- Status bar state (SHADE, KEYGUARD, SHADE_LOCKED).
  status_bar_state STRING,
  -- Active scene.
  scene STRING,
  -- Keyguard transition from.
  keyguard_transition_from STRING,
  -- Dozing.
  dozing BOOL,
  -- Bouncer showing.
  bouncer BOOL,
  -- Keyguard transition to.
  keyguard_transition_to STRING,
  -- Keyguard transition state (STARTED, RUNNING, FINISHED, CANCELED).
  keyguard_transition_state STRING,
  -- Keyguard transition value (0-1).
  keyguard_transition_value DOUBLE,
  -- Lockscreen show notifications setting.
  lockscreen_show_notifications BOOL,
  -- Lockscreen show private setting.
  lockscreen_show_private BOOL,
  -- Pinned HUN keys (comma separated).
  pinned_hun_keys STRING,
  -- Key of the notification with open guts.
  guts_key STRING,
  -- Remote input keys (comma separated).
  remote_input_keys STRING,
  -- User expanded keys (comma separated).
  user_expanded_keys STRING,
  -- Key of the notification showing the snooze menu.
  snooze_key STRING,
  -- Active overlays (comma separated).
  overlay_keys STRING
)
AS
SELECT
  snapshot_id,
  shade_expansion,
  qs_expansion,
  status_bar_state,
  scene,
  keyguard_transition_from,
  dozing,
  bouncer,
  keyguard_transition_to,
  keyguard_transition_state,
  keyguard_transition_value,
  lockscreen_show_notifications,
  lockscreen_show_private,
  pinned_hun_keys,
  guts_key,
  remote_input_keys,
  user_expanded_keys,
  snooze_key,
  overlay_keys
FROM __intrinsic_ui_hierarchy_snapshot_state;
