# Android UI Hierarchy (Views & Jetpack Compose)

The **android.ui.hierarchy** data source captures the hierarchical structure,
geometry, properties, and runtime composition lifecycle of Android app user interfaces
(both classic `View` hierarchies and modern `Jetpack Compose` composable trees).

Perfetto records UI hierarchy snapshots as delta-compressed frames, reconstructs
screen bounding boxes via hierarchical transform composition in Trace Processor, and
provides a rich 3-pane inspection viewer and multi-track timeline visualization in
the Perfetto UI.

---

## Key Features

1. **Dual Framework Support**: Unifies Android `android.view.View` hierarchies and
   Jetpack Compose semantic layout nodes and composables in a single timeline and tree.
2. **Independent Basis Geometry**: Records local placements `(x, y)` and sizes
   `(width, height)` relative to parent nodes, along with optional 3x3 local transforms
   (scale, rotation, skew, translation). Screen bounding boxes `(bounds_left, bounds_top, bounds_right, bounds_bottom)`
   are derived deterministically by Trace Processor up the node parent chain.
3. **Delta Compression & Keyframes**: Uses periodic keyframes (full trees) and
   per-frame deltas (only changed properties and removed node IDs). Moving or scrolling a
   parent node updates descendants automatically in SQL without re-emitting them.
4. **Compose Runtime Tracing**: Unsampled capture of composition passes, recomposition
   scopes, composable function calls, state changes, and scope invalidations.
5. **Causal Recomposition Analysis**: Automatically links each executed recompose scope
   with the specific snapshot state mutation that invalidated it ("Why Recomposed").
6. **Privacy & Redaction**: Built-in text redaction modes (`REDACT_SENSITIVE`,
   `REDACT_ALL`, `INCLUDE_ALL`) with `FLAG_TEXT_REDACTED` flags to safeguard sensitive
   fields (passwords, payment information, OTPs).
7. **Completeness & Backpressure Tracking**: Accurately tracks skipped frames and
   capture backpressure on a per-window basis via `android_ui_hierarchy_capture_completeness`.

---

## Configuration

To record UI hierarchy in a Perfetto trace session, add the `android.ui.hierarchy`
data source to your trace configuration:

```protobuf
data_sources: {
  config {
    name: "android.ui.hierarchy"
    ui_hierarchy_config {
      // Polling or frame-driven capture
      interval_ms: 16
      // Redaction mode: 1 = REDACT_SENSITIVE (default), 2 = REDACT_ALL, 3 = INCLUDE_ALL
      text_mode: 1
      // Target specific application packages (optional, empty = all apps)
      app_package_names: "com.example.myapp"
    }
  }
}
```

Or using the Perfetto command line:

```bash
adb shell perfetto \
  -c - --txt \
  -o /data/misc/perfetto-traces/trace.pftrace << 'EOF'
buffers: {
  size_kb: 65536
  fill_policy: RING_BUFFER
}
data_sources: {
  config {
    name: "android.ui.hierarchy"
  }
}
duration_ms: 10000
EOF
```

---

## Trace Processor SQL Schema

The stdlib module `android.ui_hierarchy` provides structured views, tables, and table
functions:

```sql
INCLUDE PERFETTO MODULE android.ui_hierarchy;
```

### 1. `android_ui_hierarchy_window`
App window versions and screen positions over time.

| Column | Type | Description |
|---|---|---|
| `id` | `LONG` | Row ID |
| `ts` | `TIMESTAMP` | Start timestamp of window version interval |
| `dur` | `DURATION` | Interval duration (-1 clamped to trace end) |
| `upid` | `JOINID(process.id)` | Owning process ID |
| `window_id` | `LONG` | Window ID |
| `window_name` | `STRING` | Window title / component name |
| `display_id` | `LONG` | Target display ID |
| `bounds_left` | `LONG` | Left screen bound (px) |
| `bounds_top` | `LONG` | Top screen bound (px) |
| `bounds_right` | `LONG` | Right screen bound (px) |
| `bounds_bottom` | `LONG` | Bottom screen bound (px) |
| `has_focus` | `BOOL` | Whether the window has user input focus |

### 2. `android_ui_hierarchy_node`
Complete, gap-free history of all UI nodes. A new row starts whenever any property of
a node changes.

| Column | Type | Description |
|---|---|---|
| `id` | `LONG` | Row ID |
| `ts` | `TIMESTAMP` | Start timestamp of node version interval |
| `dur` | `DURATION` | Duration of node version interval |
| `upid` | `JOINID(process.id)` | Owning process ID |
| `window_id` | `LONG` | Window ID |
| `node_id` | `LONG` | Stable node ID across frames |
| `parent_id` | `LONG` | Parent node ID (`NULL` for root) |
| `child_index` | `LONG` | Sibling draw/z-order index |
| `kind` | `STRING` | `'view'`, `'compose_view'`, `'compose_node'`, `'composable'` |
| `name` | `STRING` | View class or composable function name |
| `source_location` | `STRING` | Source file and line (e.g. `MyScreen.kt:42`) |
| `bounds_left` | `LONG` | Screen-space left bound (px, derived by TP) |
| `bounds_top` | `LONG` | Screen-space top bound (px, derived by TP) |
| `bounds_right` | `LONG` | Screen-space right bound (px, derived by TP) |
| `bounds_bottom` | `LONG` | Screen-space bottom bound (px, derived by TP) |
| `local_x` | `DOUBLE` | Local X offset in parent coordinate space (px) |
| `local_y` | `DOUBLE` | Local Y offset in parent coordinate space (px) |
| `width` | `LONG` | Local layout width (px) |
| `height` | `LONG` | Local layout height (px) |
| `transform` | `STRING` | Serialized 3x3 affine matrix if non-translation |
| `alpha` | `DOUBLE` | Alpha transparency (0.0 to 1.0) |
| `flags` | `LONG` | Bitmask of node flags (visible, clickable, redacted, etc.) |

### 3. Point and Query Functions

- `android_ui_hierarchy_nodes_at(ts TIMESTAMP)`: Returns all live UI nodes across all
  windows at timestamp `ts`.
- `android_ui_hierarchy_node_at_point(ts TIMESTAMP, x LONG, y LONG)`: Hit tests and
  returns the topmost clickable UI node covering screen coordinate `(x, y)` at timestamp `ts`.
- `android_ui_hierarchy_screen_text(ts TIMESTAMP)`: Reconstructs all visible text on
  screen at timestamp `ts` in reading order (top-to-bottom, left-to-right).

### 4. Compose Lifecycle & Recomposition Cause

- `android_ui_hierarchy_compose_event`: Composition lifecycle events including
  scopes, composable calls, state changes, and invalidations.
- `android_ui_hierarchy_recomposition_cause`: Joins executed recomposition scopes with
  the exact state modification that caused them to recompose:

```sql
SELECT
  scope_event_id,
  name AS composable_name,
  state_id,
  state_type,
  state_value
FROM android_ui_hierarchy_recomposition_cause
LIMIT 10;
```

---

## Perfetto UI Visualization

The Perfetto UI plugin `com.android.UiHierarchy` provides comprehensive timeline and
interactive inspection features:

### Timeline Tracks

- **Window Snapshot Tracks**: Shows keyframes (`Keyframe (N)`) and deltas (`Δ (+A -B)`).
  Selecting a snapshot opens the details panel with visible screen text preview and a
  direct shortcut to open the full UI Hierarchy viewer.
- **Compose Thread Tracks**: Per-thread composable calls and recomposition scopes nested
  by hierarchy depth. Selecting any scope reveals its duration and the exact
  invalidating state in the **Why Recomposed** panel.
- **Compose State & Invalidations Track**: Instant markers for state mutations and
  scope invalidations.
- **Frames & Completeness Track**: Visual indicators for captured window frames and
  highlighted warning markers for skipped/dropped frames.

### 3-Pane Hierarchy Viewer (`#!/ui_hierarchy`)

Navigate to the full viewer via the left sidebar (**UI Hierarchy**) or snapshot link:

1. **Surface Layout (Left Pane)**:
   - Pure HTML5 Canvas 2D layout rendering with bounding boxes and leader lines.
   - **3D Exploded Stack Mode**: Interactive 3D perspective projection with rotation
     and layer separation sliders to inspect deep view nesting and occlusion.
   - Filters for visible-only and clickable-only elements.
   - Shading modes (depth-based or kind-based).
   - Video overlay synchronization when display video (`android.display.video`) is recorded.
2. **Hierarchy Tree (Middle Pane)**:
   - Full expandable tree with depth indentation.
   - Search bar filtering by node name, test tag, text, or class name.
   - Kind badges (`VIEW`, `COMPOSE_VIEW`, `COMPOSE_NODE`, `COMPOSABLE`).
   - Redacted password / sensitive field warning indicators.
3. **Properties & Version Diff (Right Pane)**:
   - Detailed node properties (bounds, local geometry, transforms, flags, accessibility).
   - Prominent red warning banner for sensitive / password fields.
   - **Version History**: Interactive timeline of all changes to the selected node across
     the entire trace, showing property-by-property diffs (e.g. alpha changes, bounds shifts)
     with jump links to scrub the viewer to each version timestamp.
