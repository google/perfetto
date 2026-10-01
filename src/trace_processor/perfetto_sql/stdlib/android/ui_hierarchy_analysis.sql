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

-- Higher-level analysis helpers and aggregation views for android.ui.hierarchy
-- traces (Views and Jetpack Compose).

INCLUDE PERFETTO MODULE android.ui_hierarchy;

-- Compares the UI hierarchy between two timestamps.
-- Returns nodes that were added, removed, or changed between |ts1| and |ts2|.
-- Nodes with identical properties at both timestamps are excluded.
CREATE PERFETTO FUNCTION android_ui_hierarchy_diff(
  -- Baseline timestamp to compare from.
  ts1 TIMESTAMP,
  -- Target timestamp to compare to.
  ts2 TIMESTAMP
)
RETURNS TABLE(
  -- Change status: 'added', 'removed', or 'changed'.
  status STRING,
  -- Stable node id.
  node_id LONG,
  -- Window id the node belongs to.
  window_id LONG,
  -- Node kind ('view', 'compose_view', 'compose_node', 'composable').
  kind STRING,
  -- View class or composable name.
  name STRING,
  -- Comma-separated list of properties that changed (e.g. 'bounds,text'), or NULL for added/removed.
  changed_properties STRING,
  -- Text at ts1 (NULL if added or text was unset).
  old_text STRING,
  -- Text at ts2 (NULL if removed or text was unset).
  new_text STRING,
  -- Left bound in screen px at ts1.
  old_bounds_left LONG,
  -- Top bound in screen px at ts1.
  old_bounds_top LONG,
  -- Right bound in screen px at ts1.
  old_bounds_right LONG,
  -- Bottom bound in screen px at ts1.
  old_bounds_bottom LONG,
  -- Left bound in screen px at ts2.
  new_bounds_left LONG,
  -- Top bound in screen px at ts2.
  new_bounds_top LONG,
  -- Right bound in screen px at ts2.
  new_bounds_right LONG,
  -- Bottom bound in screen px at ts2.
  new_bounds_bottom LONG,
  -- Visibility at ts1.
  old_is_visible BOOL,
  -- Visibility at ts2.
  new_is_visible BOOL,
  -- Parent node id at ts1.
  old_parent_id LONG,
  -- Parent node id at ts2.
  new_parent_id LONG,
  -- Child index among siblings at ts1.
  old_child_index LONG,
  -- Child index among siblings at ts2.
  new_child_index LONG,
  -- Semantic role at ts1.
  old_role STRING,
  -- Semantic role at ts2.
  new_role STRING,
  -- Test tag at ts1.
  old_test_tag STRING,
  -- Test tag at ts2.
  new_test_tag STRING
)
AS
WITH
  t1 AS (
    SELECT n.*
    FROM android_ui_hierarchy_node n
    JOIN __intrinsic_ui_hierarchy_node raw USING (id)
    WHERE
      n.ts <= $ts1
      AND (raw.dur = -1 OR n.ts + raw.dur > $ts1)
  ),
  t2 AS (
    SELECT n.*
    FROM android_ui_hierarchy_node n
    JOIN __intrinsic_ui_hierarchy_node raw USING (id)
    WHERE
      n.ts <= $ts2
      AND (raw.dur = -1 OR n.ts + raw.dur > $ts2)
  ),
  diff AS (
    SELECT
      CASE
        WHEN t1.id IS NULL THEN 'added'
        WHEN t2.id IS NULL THEN 'removed'
        ELSE 'changed'
      END AS status,
      coalesce(t2.node_id, t1.node_id) AS node_id,
      coalesce(t2.window_id, t1.window_id) AS window_id,
      coalesce(t2.kind, t1.kind) AS kind,
      coalesce(t2.name, t1.name) AS name,
      CASE
        WHEN t1.id IS NULL
        OR t2.id IS NULL THEN NULL
        ELSE ltrim(
          iif(
            t1.bounds_left != t2.bounds_left
            OR t1.bounds_top != t2.bounds_top
            OR t1.bounds_right != t2.bounds_right
            OR t1.bounds_bottom != t2.bounds_bottom,
            ',bounds',
            ''
          )
          || iif(t1.text IS NOT t2.text, ',text', '')
          || iif(t1.is_visible IS NOT t2.is_visible, ',visibility', '')
          || iif(t1.is_focused IS NOT t2.is_focused, ',focus', '')
          || iif(t1.alpha IS NOT t2.alpha, ',alpha', '')
          || iif(
            t1.scroll_x IS NOT t2.scroll_x
            OR t1.scroll_y IS NOT t2.scroll_y,
            ',scroll',
            ''
          )
          || iif(t1.transform IS NOT t2.transform, ',transform', '')
          || iif(
            t1.lookahead_x IS NOT t2.lookahead_x
            OR t1.lookahead_y IS NOT t2.lookahead_y
            OR t1.lookahead_width IS NOT t2.lookahead_width
            OR t1.lookahead_height IS NOT t2.lookahead_height,
            ',lookahead',
            ''
          )
          || iif(t1.parent_id IS NOT t2.parent_id, ',parent', '')
          || iif(t1.child_index IS NOT t2.child_index, ',child_index', '')
          || iif(
            t1.content_description IS NOT t2.content_description,
            ',content_description',
            ''
          )
          || iif(t1.test_tag IS NOT t2.test_tag, ',test_tag', '')
          || iif(t1.role IS NOT t2.role, ',role', '')
          || iif(
            t1.state_description IS NOT t2.state_description,
            ',state_description',
            ''
          )
          || iif(t1.actions IS NOT t2.actions, ',actions', '')
          || iif(t1.properties IS NOT t2.properties, ',properties', '')
          || iif((t1.flags & ~33) != (t2.flags & ~33), ',flags', ''),
          ','
        )
      END AS changed_properties,
      t1.text AS old_text,
      t2.text AS new_text,
      t1.bounds_left AS old_bounds_left,
      t1.bounds_top AS old_bounds_top,
      t1.bounds_right AS old_bounds_right,
      t1.bounds_bottom AS old_bounds_bottom,
      t2.bounds_left AS new_bounds_left,
      t2.bounds_top AS new_bounds_top,
      t2.bounds_right AS new_bounds_right,
      t2.bounds_bottom AS new_bounds_bottom,
      t1.is_visible AS old_is_visible,
      t2.is_visible AS new_is_visible,
      t1.parent_id AS old_parent_id,
      t2.parent_id AS new_parent_id,
      t1.child_index AS old_child_index,
      t2.child_index AS new_child_index,
      t1.role AS old_role,
      t2.role AS new_role,
      t1.test_tag AS old_test_tag,
      t2.test_tag AS new_test_tag
    FROM t1
    FULL JOIN t2
      ON t1.node_id = t2.node_id
      AND t1.window_id = t2.window_id
  )
SELECT *
FROM diff
WHERE
  status != 'changed'
  OR changed_properties != ''
ORDER BY
  node_id;

-- Timeline of a UI node across all captures, showing how its properties
-- evolved over time and which properties changed in each version.
CREATE PERFETTO FUNCTION android_ui_hierarchy_node_history(
  -- Stable node id to inspect.
  node_id LONG
)
RETURNS TABLE(
  -- android_ui_hierarchy_node row id.
  id LONG,
  -- Start of the interval in which this version was live.
  ts TIMESTAMP,
  -- Duration of the interval.
  dur DURATION,
  -- Owning process.
  upid JOINID(process.id),
  -- Window id.
  window_id LONG,
  -- Stable node id.
  node_id LONG,
  -- Parent node id.
  parent_id LONG,
  -- Index among siblings.
  child_index LONG,
  -- Node kind ('view', 'compose_view', 'compose_node', 'composable').
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
  -- Effective alpha.
  alpha DOUBLE,
  -- Whether visible.
  is_visible BOOL,
  -- Whether clickable.
  is_clickable BOOL,
  -- Whether focused.
  is_focused BOOL,
  -- Text.
  text STRING,
  -- Accessibility content description.
  content_description STRING,
  -- Test tag.
  test_tag STRING,
  -- Role.
  role STRING,
  -- State description.
  state_description STRING,
  -- Comma-separated list of properties that changed compared to the previous version,
  -- or 'initial' for the first version.
  changed_properties STRING
)
AS
WITH
  versions AS (
    SELECT
      id,
      ts,
      dur,
      upid,
      window_id,
      node_id,
      parent_id,
      child_index,
      kind,
      name,
      source_location,
      bounds_left,
      bounds_top,
      bounds_right,
      bounds_bottom,
      alpha,
      flags,
      is_visible,
      is_clickable,
      is_focused,
      text,
      content_description,
      test_tag,
      role,
      state_description,
      actions,
      properties,
      scroll_x,
      scroll_y,
      transform,
      lookahead_x,
      lookahead_y,
      lookahead_width,
      lookahead_height,
      lag(bounds_left) OVER w AS prev_bounds_left,
      lag(bounds_top) OVER w AS prev_bounds_top,
      lag(bounds_right) OVER w AS prev_bounds_right,
      lag(bounds_bottom) OVER w AS prev_bounds_bottom,
      lag(text) OVER w AS prev_text,
      lag(is_visible) OVER w AS prev_is_visible,
      lag(is_focused) OVER w AS prev_is_focused,
      lag(alpha) OVER w AS prev_alpha,
      lag(scroll_x) OVER w AS prev_scroll_x,
      lag(scroll_y) OVER w AS prev_scroll_y,
      lag(transform) OVER w AS prev_transform,
      lag(lookahead_x) OVER w AS prev_lookahead_x,
      lag(lookahead_y) OVER w AS prev_lookahead_y,
      lag(lookahead_width) OVER w AS prev_lookahead_width,
      lag(lookahead_height) OVER w AS prev_lookahead_height,
      lag(parent_id) OVER w AS prev_parent_id,
      lag(child_index) OVER w AS prev_child_index,
      lag(content_description) OVER w AS prev_content_description,
      lag(test_tag) OVER w AS prev_test_tag,
      lag(role) OVER w AS prev_role,
      lag(state_description) OVER w AS prev_state_description,
      lag(actions) OVER w AS prev_actions,
      lag(properties) OVER w AS prev_properties,
      lag(flags) OVER w AS prev_flags,
      row_number() OVER w AS rn
    FROM android_ui_hierarchy_node
    WHERE
      node_id = $node_id
    WINDOW
      w AS (PARTITION BY window_id, node_id ORDER BY ts)
  )
SELECT
  id,
  ts,
  dur,
  upid,
  window_id,
  node_id,
  parent_id,
  child_index,
  kind,
  name,
  source_location,
  bounds_left,
  bounds_top,
  bounds_right,
  bounds_bottom,
  alpha,
  is_visible,
  is_clickable,
  is_focused,
  text,
  content_description,
  test_tag,
  role,
  state_description,
  CASE
    WHEN rn = 1 THEN 'initial'
    ELSE ltrim(
      iif(
        bounds_left != prev_bounds_left
        OR bounds_top != prev_bounds_top
        OR bounds_right != prev_bounds_right
        OR bounds_bottom != prev_bounds_bottom,
        ',bounds',
        ''
      )
      || iif(text IS NOT prev_text, ',text', '')
      || iif(is_visible IS NOT prev_is_visible, ',visibility', '')
      || iif(is_focused IS NOT prev_is_focused, ',focus', '')
      || iif(alpha IS NOT prev_alpha, ',alpha', '')
      || iif(
        scroll_x IS NOT prev_scroll_x
        OR scroll_y IS NOT prev_scroll_y,
        ',scroll',
        ''
      )
      || iif(transform IS NOT prev_transform, ',transform', '')
      || iif(
        lookahead_x IS NOT prev_lookahead_x
        OR lookahead_y IS NOT prev_lookahead_y
        OR lookahead_width IS NOT prev_lookahead_width
        OR lookahead_height IS NOT prev_lookahead_height,
        ',lookahead',
        ''
      )
      || iif(parent_id IS NOT prev_parent_id, ',parent', '')
      || iif(child_index IS NOT prev_child_index, ',child_index', '')
      || iif(
        content_description IS NOT prev_content_description,
        ',content_description',
        ''
      )
      || iif(test_tag IS NOT prev_test_tag, ',test_tag', '')
      || iif(role IS NOT prev_role, ',role', '')
      || iif(
        state_description IS NOT prev_state_description,
        ',state_description',
        ''
      )
      || iif(actions IS NOT prev_actions, ',actions', '')
      || iif(properties IS NOT prev_properties, ',properties', '')
      || iif((flags & ~33) != (prev_flags & ~33), ',flags', ''),
      ','
    )
  END AS changed_properties
FROM versions
ORDER BY
  ts;

-- Per-composable recomposition statistics (count, total time, average time,
-- and top invalidation causes). Identifies UI recomposition hotspots and
-- what state changes triggered them.
CREATE PERFETTO VIEW android_ui_hierarchy_recomposition_stats(
  -- Owning process.
  upid JOINID(process.id),
  -- Composable / scope name ("fn (File.kt:line)").
  composable_name STRING,
  -- Total number of times this composable scope executed.
  recomposition_count LONG,
  -- Total time spent executing this scope in nanoseconds.
  total_dur_ns DURATION,
  -- Average duration per scope execution in nanoseconds.
  avg_dur_ns DOUBLE,
  -- Most frequent invalidating state type/source for this composable.
  top_cause STRING,
  -- Comma-separated list of all distinct invalidation causes seen.
  causes STRING
)
AS
WITH
  scopes AS (
    SELECT
      upid,
      name AS composable_name,
      count(*) AS recomposition_count,
      sum(dur) AS total_dur_ns,
      CAST(avg(dur) AS DOUBLE) AS avg_dur_ns
    FROM android_ui_hierarchy_compose_event
    WHERE
      type = 'scope'
    GROUP BY
      upid,
      name
  ),
  cause_counts AS (
    SELECT
      upid,
      name AS composable_name,
      coalesce(state_type, 'explicit/parent') AS cause,
      count(*) AS cnt,
      row_number() OVER (PARTITION BY upid, name ORDER BY count(*) DESC) AS rnk
    FROM android_ui_hierarchy_recomposition_cause
    GROUP BY
      upid,
      name,
      cause
  ),
  top_causes AS (
    SELECT upid, composable_name, cause AS top_cause
    FROM cause_counts
    WHERE
      rnk = 1
  ),
  all_causes AS (
    SELECT
      upid,
      name AS composable_name,
      group_concat(DISTINCT coalesce(state_type, 'explicit/parent')) AS causes
    FROM android_ui_hierarchy_recomposition_cause
    GROUP BY
      upid,
      name
  )
SELECT
  s.upid,
  s.composable_name,
  s.recomposition_count,
  s.total_dur_ns,
  s.avg_dur_ns,
  tc.top_cause,
  ac.causes
FROM scopes s
LEFT JOIN top_causes tc
  ON s.upid IS tc.upid
  AND s.composable_name IS tc.composable_name
LEFT JOIN all_causes ac
  ON s.upid IS ac.upid
  AND s.composable_name IS ac.composable_name
ORDER BY
  s.total_dur_ns DESC;

-- Compose animations: one row per animation run, including its label,
-- time span, end reason, and number of emitted frames.
CREATE PERFETTO VIEW android_ui_hierarchy_animation(
  -- Row id (start event id).
  id LONG,
  -- Animation object id.
  animation_id LONG,
  -- Parent animation or transition id (NULL for root animations).
  parent_animation_id LONG,
  -- Owning process.
  upid JOINID(process.id),
  -- Animation label.
  label STRING,
  -- Start timestamp.
  start_ts TIMESTAMP,
  -- End timestamp.
  end_ts TIMESTAMP,
  -- Duration of the animation.
  dur DURATION,
  -- End reason ('finished', 'bounds_reached', 'cancelled', 'in_progress').
  end_reason STRING,
  -- Number of animation frame events recorded.
  frame_count LONG,
  -- Value range ("initial -> target").
  value_range STRING,
  -- Animation specification.
  spec STRING
)
AS
WITH
  starts AS (
    SELECT
      id AS start_id,
      ts AS start_ts,
      upid,
      object_id,
      parent_id AS parent_animation_id,
      name AS label,
      value AS value_range,
      spec,
      lead(ts) OVER (PARTITION BY upid, object_id ORDER BY ts) AS next_start_ts
    FROM android_ui_hierarchy_compose_event
    WHERE
      type = 'animation_start'
  ),
  ends AS (
    SELECT
      id AS end_id,
      ts AS end_ts,
      upid,
      object_id,
      action,
      CASE coalesce(action, 0)
        WHEN 0 THEN 'finished'
        WHEN 1 THEN 'bounds_reached'
        WHEN 2 THEN 'cancelled'
        ELSE 'unknown'
      END AS end_reason
    FROM android_ui_hierarchy_compose_event
    WHERE
      type = 'animation_end'
  ),
  matched_ends AS (
    SELECT
      s.start_id,
      e.end_id,
      e.end_ts,
      e.end_reason,
      row_number() OVER (PARTITION BY s.start_id ORDER BY e.end_ts ASC) AS rnk
    FROM starts s
    JOIN ends e
      ON e.upid IS s.upid
      AND e.object_id = s.object_id
      AND e.end_ts >= s.start_ts
      AND (s.next_start_ts IS NULL OR e.end_ts < s.next_start_ts)
  ),
  primary_ends AS (
    SELECT start_id, end_id, end_ts, end_reason FROM matched_ends WHERE rnk = 1
  ),
  frames AS (
    SELECT s.start_id, count(*) AS frame_count
    FROM starts s
    JOIN android_ui_hierarchy_compose_event f
      ON f.type = 'animation_frame'
      AND f.upid IS s.upid
      AND f.object_id = s.object_id
      AND f.ts >= s.start_ts
      AND (s.next_start_ts IS NULL OR f.ts < s.next_start_ts)
    GROUP BY
      s.start_id
  )
SELECT
  s.start_id AS id,
  s.object_id AS animation_id,
  s.parent_animation_id,
  s.upid,
  s.label,
  s.start_ts,
  coalesce(pe.end_ts, trace_end()) AS end_ts,
  coalesce(pe.end_ts, trace_end()) - s.start_ts AS dur,
  coalesce(pe.end_reason, 'in_progress') AS end_reason,
  coalesce(f.frame_count, 0) AS frame_count,
  s.value_range,
  s.spec
FROM starts s
LEFT JOIN primary_ends pe
  ON s.start_id = pe.start_id
LEFT JOIN frames f
  ON s.start_id = f.start_id
ORDER BY
  s.start_ts;
