# UI Hierarchy Analysis Guide (Android Views & Jetpack Compose)

The `android.ui.hierarchy` data source captures the full in-app UI tree (Android Views and Jetpack Compose) across all windows, with screen coordinates, layout bounds, semantics, accessibility attributes, text, modifiers, and runtime events (recompositions, state writes/changes, animations, layout passes, input).

This document serves as a reference and query cookbook for AI agents and human developers analyzing UI behavior, animations, layout shifts, jank, and recomposition bottlenecks with Trace Processor and PerfettoSQL.

---

## 1. Modules and Tables

Include the stdlib modules in your PerfettoSQL queries:

```sql
INCLUDE PERFETTO MODULE android.ui_hierarchy;
INCLUDE PERFETTO MODULE android.ui_hierarchy_analysis;
```

### Core Hierarchy & Snapshot Views (`android.ui_hierarchy`)

| View / Table | Description |
|---|---|
| `android_ui_hierarchy_snapshot` | One row per captured packet (`is_keyframe`, `frame_id`, `changed_node_count`, `removed_node_count`, `text_mode`). |
| `android_ui_hierarchy_window` | Interval table of app windows (`window_id`, `window_name`, `display_id`, `bounds_left..bottom`, `has_focus`). |
| `android_ui_hierarchy_node` | Interval table of UI nodes (`node_id`, `parent_id`, `kind`, `name`, `bounds_left..bottom`, `local_x..height`, `transform`, `lookahead_x..height`, `text`, `role`, `test_tag`, flags). Materialized per property change: valid over `[ts, ts + dur)`. |
| `android_ui_hierarchy_compose_event` | Stream of Compose runtime events (`composition`, `scope`, `scope_invalidated`, `state_changed`, `animation_start..frame..end`, `scroll`, etc.). |
| `android_ui_hierarchy_recomposition_cause` | Joins every recompose scope execution with the invalidations (and causing state) that triggered it. |
| `android_ui_hierarchy_capture_completeness` | Frame accounting (`emitted_frames`, `skipped_frames`) per window. `skipped_frames = 0` guarantees no drawn state was missed. |

### Helper & Analysis Functions (`android.ui_hierarchy_analysis` & `android.ui_hierarchy`)

| Function / View | Description |
|---|---|
| `android_ui_hierarchy_diff(ts1, ts2)` | Compares the UI tree between two timestamps. Returns added, removed, and changed nodes with a comma-separated list of changed properties (`bounds`, `text`, `visibility`, `parent`, etc.). |
| `android_ui_hierarchy_node_history(node_id)` | Complete chronological evolution of a single node across all its versions, identifying what changed at each step. |
| `android_ui_hierarchy_recomposition_stats` | Aggregated per-composable recomposition metrics: total count, total execution time, average duration, and top invalidation causes. |
| `android_ui_hierarchy_animation` | One row per animation run: label, start and end timestamps, duration, end reason (`finished`, `bounds_reached`, `cancelled`), and frame count. |
| `android_ui_hierarchy_screen_text(ts)` | Compact, reading-order dump of visible text, roles, and test tags at timestamp `ts` for LLM inspection. |
| `android_ui_hierarchy_nodes_at(ts)` | The complete set of nodes live at timestamp `ts`. |
| `android_ui_hierarchy_node_at_point(ts, x, y)` | Deepest visible node enclosing screen coordinates `(x, y)` at timestamp `ts` (hit testing). |

---

## 2. Example Queries and Recipes for AI Agents

### Recipe 1: "What text and controls were visible on screen at timestamp T?"

To produce a compact, reading-order text summary of the UI for language models or human inspection:

```sql
INCLUDE PERFETTO MODULE android.ui_hierarchy;

SELECT
  role,
  label,
  test_tag,
  bounds_left,
  bounds_top,
  bounds_right,
  bounds_bottom,
  is_clickable
FROM android_ui_hierarchy_screen_text(:ts)
ORDER BY bounds_top, bounds_left;
```

---

### Recipe 2: "What changed between timestamps T1 and T2?"

To detect all added, removed, or modified UI nodes between two snapshots (e.g. before and after a user click or screen transition):

```sql
INCLUDE PERFETTO MODULE android.ui_hierarchy_analysis;

SELECT
  status,
  node_id,
  name,
  kind,
  changed_properties,
  old_text,
  new_text,
  old_bounds_left,
  old_bounds_top,
  new_bounds_left,
  new_bounds_top,
  old_is_visible,
  new_is_visible
FROM android_ui_hierarchy_diff(:ts1, :ts2)
ORDER BY node_id;
```

---

### Recipe 3: "Find why button / node X moved or changed"

To trace the complete history of an element (e.g. `node_id = 42`) across the entire trace:

```sql
INCLUDE PERFETTO MODULE android.ui_hierarchy_analysis;

SELECT
  ts,
  dur,
  name,
  text,
  bounds_left,
  bounds_top,
  bounds_right,
  bounds_bottom,
  is_visible,
  changed_properties
FROM android_ui_hierarchy_node_history(:node_id)
ORDER BY ts;
```

---

### Recipe 4: "Which composable recomposes most and why?"

To identify recomposition hotspots, CPU consumption in Compose scopes, and the primary state variables invalidating them:

```sql
INCLUDE PERFETTO MODULE android.ui_hierarchy_analysis;

SELECT
  composable_name,
  recomposition_count,
  total_dur_ns,
  avg_dur_ns,
  top_cause,
  causes
FROM android_ui_hierarchy_recomposition_stats
ORDER BY total_dur_ns DESC
LIMIT 20;
```

To drill into individual invalidating state instances and values for a specific composable:

```sql
INCLUDE PERFETTO MODULE android.ui_hierarchy;

SELECT
  ts,
  dur,
  scope_id,
  state_type,
  state_value
FROM android_ui_hierarchy_recomposition_cause
WHERE name LIKE '%MyComposable%'
ORDER BY ts;
```

---

### Recipe 5: "What animations ran, how long did they take, and why did they stop?"

To inspect animation runs, their specs, duration, frame counts, and completion status:

```sql
INCLUDE PERFETTO MODULE android.ui_hierarchy_analysis;

SELECT
  animation_id,
  label,
  start_ts,
  end_ts,
  dur,
  end_reason,
  frame_count,
  value_range,
  spec
FROM android_ui_hierarchy_animation
ORDER BY start_ts;
```

To inspect individual animation frame values over time:

```sql
INCLUDE PERFETTO MODULE android.ui_hierarchy;

SELECT
  ts,
  play_time_ns,
  value
FROM android_ui_hierarchy_compose_event
WHERE type = 'animation_frame' AND object_id = :animation_id
ORDER BY ts;
```

---

### Recipe 6: "Correlate UI hierarchy with SurfaceFlinger layers, FrameTimeline (jank), and Video frames"

Every snapshot carries `frame_id` (Choreographer vsync id) and screen-space coordinates `(bounds_left..bottom)`. This enables joins with SurfaceFlinger, FrameTimeline, and video frames:

```sql
INCLUDE PERFETTO MODULE android.ui_hierarchy;
INCLUDE PERFETTO MODULE android.frames.timeline;

-- Join UI snapshots with FrameTimeline to identify which UI state was present during janky frames
SELECT
  s.ts AS snapshot_ts,
  s.frame_id,
  f.jank_type,
  f.dur AS frame_dur,
  w.window_name
FROM android_ui_hierarchy_snapshot s
JOIN actual_frame_timeline_slice f ON f.vsync = s.frame_id
JOIN android_ui_hierarchy_window w ON w.window_id = s.id AND w.ts <= s.ts AND (w.dur = -1 OR w.ts + w.dur > s.ts)
WHERE f.jank_type != 'None'
ORDER BY s.ts;
```

---

### Recipe 7: "What UI element did the user tap?"

When input tracing (`android.input.inputevent`) or winscope input events are present, hit-test the tapped screen coordinate:

```sql
INCLUDE PERFETTO MODULE android.ui_hierarchy;

-- Find the deepest interactive node hit by touch at coordinates (x=450, y=920) at timestamp T
SELECT
  node_id,
  kind,
  name,
  text,
  test_tag,
  role
FROM android_ui_hierarchy_node_at_point(:touch_ts, :touch_x, :touch_y);
```
