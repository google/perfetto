# Copyright (C) 2026 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

from python.generators.trace_processor_table.public import Column as C
from python.generators.trace_processor_table.public import CppAccess
from python.generators.trace_processor_table.public import CppAccessDuration
from python.generators.trace_processor_table.public import CppDouble
from python.generators.trace_processor_table.public import CppInt32
from python.generators.trace_processor_table.public import CppInt64
from python.generators.trace_processor_table.public import CppOptional
from python.generators.trace_processor_table.public import CppString
from python.generators.trace_processor_table.public import CppTableId
from python.generators.trace_processor_table.public import CppUint32
from python.generators.trace_processor_table.public import Table
from python.generators.trace_processor_table.public import TableDoc
from src.trace_processor.tables.metadata_tables import PROCESS_TABLE
from src.trace_processor.tables.metadata_tables import THREAD_TABLE

# One row per UiHierarchySnapshot packet (keyframe or delta).
UI_HIERARCHY_SNAPSHOT_TABLE = Table(
    python_module=__file__,
    class_name='UiHierarchySnapshotTable',
    sql_name='__intrinsic_ui_hierarchy_snapshot',
    columns=[
        C('ts',
          CppInt64(),
          cpp_access=CppAccess.READ,
          cpp_access_duration=CppAccessDuration.POST_FINALIZATION),
        C('upid', CppOptional(CppTableId(PROCESS_TABLE))),
        C('is_keyframe', CppInt32()),
        C('vsync_id', CppOptional(CppInt64())),
        C('text_mode', CppOptional(CppInt32())),
        C('changed_node_count', CppInt32()),
        C('removed_node_count', CppInt32()),
    ],
    tabledoc=TableDoc(
        doc='''
          One row per android.ui.hierarchy snapshot packet. Keyframes carry
          the full tree; deltas carry only changed/removed nodes. Use the
          interval tables (__intrinsic_ui_hierarchy_node / _window) to get the
          full tree at any point in time.
        ''',
        group='Android',
        columns={
            'ts': 'Timestamp of the capture.',
            'upid': 'Process that emitted the snapshot.',
            'is_keyframe': '1 if the snapshot carries the full tree.',
            'vsync_id': 'Choreographer vsync id of the captured frame, if '
                        'known.',
            'text_mode': 'UiHierarchyConfig.TextMode actually applied.',
            'changed_node_count': 'Nodes present in the packet.',
            'removed_node_count': 'Nodes explicitly removed by the packet.',
        }))

# Interval table: one row per distinct version of a window.
UI_HIERARCHY_WINDOW_TABLE = Table(
    python_module=__file__,
    class_name='UiHierarchyWindowTable',
    sql_name='__intrinsic_ui_hierarchy_window',
    columns=[
        C('ts',
          CppInt64(),
          cpp_access=CppAccess.READ,
          cpp_access_duration=CppAccessDuration.POST_FINALIZATION),
        C('dur', CppInt64(), cpp_access=CppAccess.READ_AND_LOW_PERF_WRITE),
        C('upid', CppOptional(CppTableId(PROCESS_TABLE))),
        C('window_id', CppInt64()),
        C('title', CppOptional(CppString())),
        C('display_id', CppInt32()),
        C('bounds_left', CppInt32()),
        C('bounds_top', CppInt32()),
        C('bounds_right', CppInt32()),
        C('bounds_bottom', CppInt32()),
        C('has_focus', CppInt32()),
        C('pending_work_mask', CppOptional(CppUint32())),
    ],
    tabledoc=TableDoc(
        doc='''
          Interval table of app windows captured by android.ui.hierarchy.
          A new row is started whenever any window property changes.
        ''',
        group='Android',
        columns={
            'ts': 'Start of the interval in which this version was live.',
            'dur': 'Duration of the interval; -1 if still live at trace end.',
            'upid': 'Owning process.',
            'window_id': 'Producer-assigned stable window id.',
            'title': 'Window title.',
            'display_id': 'Logical display id.',
            'bounds_left': 'Window bounds in screen px.',
            'bounds_top': 'Window bounds in screen px.',
            'bounds_right': 'Window bounds in screen px.',
            'bounds_bottom': 'Window bounds in screen px.',
            'has_focus': '1 if the window has input focus.',
            'pending_work_mask': 'Bitmask of PendingWork.',
        }))

# Interval table: one row per distinct version of a node. Every change of any
# node property starts a new row, so this table is the complete, gap-free
# history of the UI tree.
UI_HIERARCHY_NODE_TABLE = Table(
    python_module=__file__,
    class_name='UiHierarchyNodeTable',
    sql_name='__intrinsic_ui_hierarchy_node',
    columns=[
        C('ts',
          CppInt64(),
          cpp_access=CppAccess.READ,
          cpp_access_duration=CppAccessDuration.POST_FINALIZATION),
        C('dur', CppInt64(), cpp_access=CppAccess.READ_AND_LOW_PERF_WRITE),
        C('upid', CppOptional(CppTableId(PROCESS_TABLE))),
        C('window_id', CppInt64()),
        C('node_id', CppInt64()),
        C('parent_node_id', CppOptional(CppInt64())),
        C('child_index', CppInt32()),
        C('kind', CppInt32()),
        C('name', CppOptional(CppString())),
        C('source_location', CppOptional(CppString())),
        C('bounds_left', CppInt32()),
        C('bounds_top', CppInt32()),
        C('bounds_right', CppInt32()),
        C('bounds_bottom', CppInt32()),
        C('local_x', CppDouble()),
        C('local_y', CppDouble()),
        C('width', CppInt32()),
        C('height', CppInt32()),
        C('transform', CppOptional(CppString())),
        C('lookahead_x', CppOptional(CppInt32())),
        C('lookahead_y', CppOptional(CppInt32())),
        C('lookahead_width', CppOptional(CppInt32())),
        C('lookahead_height', CppOptional(CppInt32())),
        C('alpha', CppDouble()),
        C('is_effectively_visible', CppInt32()),
        C('effective_alpha', CppDouble()),
        C('flags', CppInt64()),
        C('text', CppOptional(CppString())),
        C('content_description', CppOptional(CppString())),
        C('test_tag', CppOptional(CppString())),
        C('role', CppOptional(CppString())),
        C('state_description', CppOptional(CppString())),
        C('actions', CppOptional(CppString())),
        C('properties', CppOptional(CppString())),
        C('elevation', CppOptional(CppDouble())),
        C('scroll_x', CppOptional(CppInt32())),
        C('scroll_y', CppOptional(CppInt32())),
        C('hashcode', CppOptional(CppInt32())),
        C('clip_left', CppOptional(CppInt32())),
        C('clip_top', CppOptional(CppInt32())),
        C('clip_right', CppOptional(CppInt32())),
        C('clip_bottom', CppOptional(CppInt32())),
        C('draw_order', CppOptional(CppInt32())),
        C('z', CppOptional(CppDouble())),
        C('semantic_key', CppOptional(CppString())),
        C('semantic_role', CppOptional(CppString())),
    ],
    tabledoc=TableDoc(
        doc='''
          Interval table of UI nodes (Views, Compose semantics nodes and
          composables) captured by android.ui.hierarchy. Deltas are
          materialized: a row is live over [ts, ts + dur) and a new row starts
          whenever any property of the node changes, so the full tree at any
          time T is `WHERE ts <= T AND (dur = -1 OR ts + dur > T)`.
        ''',
        group='Android',
        columns={
            'ts':
                'Start of the interval in which this version was live.',
            'dur':
                'Duration of the interval; -1 if still live at trace end.',
            'upid':
                'Owning process.',
            'window_id':
                'Window this node belongs to.',
            'node_id':
                'Producer-assigned id, stable for the node lifetime.',
            'parent_node_id':
                'node_id of the parent; NULL for the root.',
            'child_index':
                'Index among siblings (z/draw order).',
            'kind':
                'UiNode.Kind (1=VIEW, 2=COMPOSE_VIEW, 3=COMPOSE_NODE, '
                '4=COMPOSABLE, 5=COMPOSE_LAYOUT).',
            'name':
                'View class or composable name.',
            'source_location':
                'Source file:line, if known.',
            'bounds_left':
                'Screen bounds (px), derived: AABB of the node rect under the composed local->screen transforms.',
            'bounds_top':
                'Screen bounds (px), derived.',
            'bounds_right':
                'Screen bounds (px), derived.',
            'bounds_bottom':
                'Screen bounds (px), derived.',
            'local_x':
                'Recorded offset in the parent (px).',
            'local_y':
                'Recorded offset in the parent (px).',
            'width':
                'Recorded size (px), before transforms.',
            'height':
                'Recorded size (px), before transforms.',
            'transform':
                'Recorded local->parent 3x3 matrix (row-major, space separated) when not a pure translation.',
            'lookahead_x':
                'Lookahead (target) offset in the parent.',
            'lookahead_y':
                'Lookahead (target) offset in the parent.',
            'lookahead_width':
                'Lookahead (target) size.',
            'lookahead_height':
                'Lookahead (target) size.',
            'alpha':
                'Effective alpha.',
            'is_effectively_visible':
                '1 if this node and all its ancestors are visible.',
            'effective_alpha':
                'Alpha multiplied by the effective alpha of its parent.',
            'flags':
                'Bitmask of UiNode.Flag.',
            'text':
                'Displayed/edited text (possibly redacted).',
            'content_description':
                'Accessibility content description.',
            'test_tag':
                'Compose testTag or View resource entry name.',
            'role':
                'Semantic role (Button, Checkbox, ...).',
            'state_description':
                'Accessibility state description.',
            'actions':
                'Comma separated list of semantic actions.',
            'properties':
                'Additional name=value properties, newline '
                'separated.',
            'elevation':
                'View elevation / z in px.',
            'scroll_x':
                'Scroll offset X px.',
            'scroll_y':
                'Scroll offset Y px.',
            'hashcode':
                'System.identityHashCode of the View.',
            'clip_left':
                'Drawn and clipped left bound in screen px.',
            'clip_top':
                'Drawn and clipped top bound in screen px.',
            'clip_right':
                'Drawn and clipped right bound in screen px.',
            'clip_bottom':
                'Drawn and clipped bottom bound in screen px.',
            'draw_order':
                'Window-global paint order.',
            'z':
                'View.getZ() or Compose effective zIndex/elevation.',
            'semantic_key':
                'Semantic key for anchor nodes.',
            'semantic_role':
                'Normalized semantic role.',
        }))

# One row per UiWindow message: frame accounting for completeness checks.
UI_HIERARCHY_WINDOW_FRAME_TABLE = Table(
    python_module=__file__,
    class_name='UiHierarchyWindowFrameTable',
    sql_name='__intrinsic_ui_hierarchy_window_frame',
    columns=[
        C('ts', CppInt64()),
        C('upid', CppOptional(CppTableId(PROCESS_TABLE))),
        C('snapshot_id', CppTableId(UI_HIERARCHY_SNAPSHOT_TABLE)),
        C('window_id', CppInt64()),
        C('frame_number', CppOptional(CppInt64())),
        C('skipped_frames', CppInt64()),
        C('is_removed', CppInt32()),
        C('vsync_id', CppOptional(CppInt64())),
        C('window_type', CppOptional(CppInt32())),
    ],
    tabledoc=TableDoc(
        doc='''
          One row per window per snapshot packet. frame_number counts every
          draw of the window (captured or not); skipped_frames counts draws
          since the previous row that were not captured. Unchanged frames
          produce no row, so frame_number gaps with skipped_frames = 0 mean
          "drawn, identical tree".
        ''',
        group='Android',
        columns={
            'ts':
                'Timestamp of the snapshot.',
            'upid':
                'Owning process.',
            'snapshot_id':
                'Snapshot packet row.',
            'window_id':
                'Window id.',
            'frame_number':
                'Draw count of the window since tracing started.',
            'skipped_frames':
                'Draws not captured since the previous row.',
            'is_removed':
                '1 if the window was removed.',
            'vsync_id':
                'Choreographer vsync id of the draw (joins FrameTimeline).',
            'window_type':
                'WindowManager.LayoutParams.type.',
        }))

# One row per Compose runtime event (UiHierarchyEvents).
UI_HIERARCHY_EVENT_TABLE = Table(
    python_module=__file__,
    class_name='UiHierarchyEventTable',
    sql_name='__intrinsic_ui_hierarchy_event',
    columns=[
        C('ts',
          CppInt64(),
          cpp_access=CppAccess.READ,
          cpp_access_duration=CppAccessDuration.POST_FINALIZATION),
        C('dur', CppInt64()),
        C('upid', CppOptional(CppTableId(PROCESS_TABLE))),
        C('utid', CppOptional(CppTableId(THREAD_TABLE))),
        C('type', CppInt32()),
        C('name', CppOptional(CppString())),
        C('scope_id', CppOptional(CppInt64())),
        C('state_id', CppOptional(CppInt64())),
        C('value', CppOptional(CppString())),
        C('depth', CppInt32()),
        C('dirty1', CppOptional(CppInt64())),
        C('dirty2', CppOptional(CppInt64())),
        C('composition_id', CppOptional(CppInt64())),
        C('node_id', CppOptional(CppInt64())),
        C('parent_id', CppOptional(CppInt64())),
        C('node_index', CppOptional(CppInt32())),
        C('from_index', CppOptional(CppInt32())),
        C('x', CppOptional(CppDouble())),
        C('y', CppOptional(CppDouble())),
        C('width', CppOptional(CppInt32())),
        C('height', CppOptional(CppInt32())),
        C('min_width', CppOptional(CppInt32())),
        C('max_width', CppOptional(CppInt32())),
        C('min_height', CppOptional(CppInt32())),
        C('max_height', CppOptional(CppInt32())),
        C('object_id', CppOptional(CppInt64())),
        C('frame_time_ns', CppOptional(CppInt64())),
        C('play_time_ns', CppOptional(CppInt64())),
        C('pointer_id', CppOptional(CppInt32())),
        C('action', CppOptional(CppInt32())),
        C('consumed', CppOptional(CppUint32())),
        C('delta', CppOptional(CppDouble())),
        C('consumed_delta', CppOptional(CppDouble())),
        C('spec', CppOptional(CppString())),
        C('window_id', CppOptional(CppInt32())),
        C('input_event_id', CppOptional(CppInt32())),
        C('is_lookahead', CppOptional(CppUint32())),
        C('semantic_key', CppOptional(CppString())),
        C('args', CppOptional(CppString())),
    ],
    tabledoc=TableDoc(
        doc='''
          Every Compose event: composition passes, recompose scopes (with
          invalidation causes), state reads/writes/changes, composable calls,
          frames, effects, layout tree changes, invalidations, measure/place/
          draw, input, focus, animations, scrolls and lazy items. Not
          sampled.
        ''',
        group='Android',
        columns={
            'ts':
                'Event timestamp (start for slice-like events).',
            'dur':
                'Duration for slice-like events, else 0.',
            'upid':
                'Owning process.',
            'utid':
                'Thread the event happened on.',
            'type':
                'UiEvent.Type enum value (see ui_hierarchy.proto); use '
                'android_ui_hierarchy_compose_event.type for names.',
            'name':
                'Composable/scope name ("fn (File.kt:line)") or state type.',
            'scope_id':
                'Recompose scope id (stable per scope).',
            'state_id':
                'State object id (stable per object).',
            'value':
                'State value description (subject to text_mode).',
            'depth':
                'Nesting depth.',
            'dirty1':
                'Compose $changed bits (composable calls).',
            'dirty2':
                'Compose $changed bits, second word.',
            'composition_id':
                'Composition id (composition passes).',
            'node_id':
                'Layout node id (== node id in snapshots).',
            'parent_id':
                'Parent layout node id (tree events).',
            'node_index':
                'Child index (insert/remove/move target, lazy item index).',
            'from_index':
                'Source index for moves.',
            'x':
                'X position px (place: in parent; input: in window).',
            'y':
                'Y position px.',
            'width':
                'Width px (measure result / place).',
            'height':
                'Height px.',
            'min_width':
                'Measure constraint.',
            'max_width':
                'Measure constraint (-1 = unbounded).',
            'min_height':
                'Measure constraint.',
            'max_height':
                'Measure constraint (-1 = unbounded).',
            'object_id':
                'Animation / transition / remember observer id.',
            'frame_time_ns':
                'Recomposer frame time (TYPE_FRAME).',
            'play_time_ns':
                'Animation / transition play time.',
            'pointer_id':
                'Pointer id (pointer input).',
            'action':
                'Type-specific action / reason code.',
            'consumed':
                'Whether the input / scroll was consumed (0/1).',
            'delta':
                'Requested scroll delta.',
            'consumed_delta':
                'Consumed scroll delta.',
            'spec':
                'Animation spec description.',
            'window_id':
                'Window id (owner-level events).',
            'input_event_id':
                'MotionEvent/KeyEvent id (joins android_input_events).',
            'is_lookahead':
                '1 for lookahead measure/place passes.',
            'semantic_key':
                'Semantic key for app state / interaction events.',
            'args':
                'Comma-separated extra args for events.',
        }))

# System UI state extracted from UiHierarchySnapshot and UiStateEvent.
UI_HIERARCHY_SYSUI_STATE_TABLE = Table(
    python_module=__file__,
    class_name='SysUiStateTable',
    sql_name='__intrinsic_ui_hierarchy_sysui_state',
    columns=[
        C('ts',
          CppInt64(),
          cpp_access=CppAccess.READ,
          cpp_access_duration=CppAccessDuration.POST_FINALIZATION),
        C('upid', CppOptional(CppTableId(PROCESS_TABLE))),
        C('field', CppString()),
        C('value_float', CppOptional(CppDouble())),
        C('value_bool', CppOptional(CppInt32())),
        C('value_string', CppOptional(CppString())),
        C('value_key', CppOptional(CppString())),
    ],
    tabledoc=TableDoc(
        doc='''
          SystemUI state changes (UiStateEvent), one row per changed field.
        ''',
        group='Android UI Hierarchy',
        columns={
            'ts': 'Timestamp of the state change.',
            'upid': 'Process that reported the change.',
            'field': 'Name of the state field.',
            'value_float': 'Float value.',
            'value_bool': 'Boolean value.',
            'value_string': 'String value.',
            'value_key': 'Set members (notification keys or overlay names).',
        }))

# System UI state joined per snapshot.
UI_HIERARCHY_SNAPSHOT_STATE_TABLE = Table(
    python_module=__file__,
    class_name='UiHierarchySnapshotStateTable',
    sql_name='__intrinsic_ui_hierarchy_snapshot_state',
    columns=[
        C('snapshot_id', CppTableId(UI_HIERARCHY_SNAPSHOT_TABLE)),
        C('shade_expansion', CppOptional(CppDouble())),
        C('qs_expansion', CppOptional(CppDouble())),
        C('status_bar_state', CppOptional(CppString())),
        C('scene', CppOptional(CppString())),
        C('keyguard_transition_from', CppOptional(CppString())),
        C('dozing', CppOptional(CppInt32())),
        C('bouncer', CppOptional(CppInt32())),
        C('keyguard_transition_to', CppOptional(CppString())),
        C('keyguard_transition_state', CppOptional(CppString())),
        C('keyguard_transition_value', CppOptional(CppDouble())),
        C('lockscreen_show_notifications', CppOptional(CppInt32())),
        C('lockscreen_show_private', CppOptional(CppInt32())),
        C('pinned_hun_keys', CppOptional(CppString())),
        C('guts_key', CppOptional(CppString())),
        C('remote_input_keys', CppOptional(CppString())),
        C('user_expanded_keys', CppOptional(CppString())),
        C('snooze_key', CppOptional(CppString())),
        C('overlay_keys', CppOptional(CppString())),
    ],
    tabledoc=TableDoc(
        doc='''
          SysUi state for each snapshot.
        ''',
        group='Android UI Hierarchy',
        columns={
            'snapshot_id': 'Snapshot ID.',
            'shade_expansion': 'Shade expansion.',
            'qs_expansion': 'QS expansion.',
            'status_bar_state': 'Status bar state.',
            'scene': 'Scene.',
            'keyguard_transition_from': 'Keyguard transition from.',
            'dozing': 'Dozing.',
            'bouncer': 'Bouncer.',
            'keyguard_transition_to': 'Keyguard transition to.',
            'keyguard_transition_state': 'Keyguard transition state.',
            'keyguard_transition_value': 'Keyguard transition value.',
            'lockscreen_show_notifications': 'Lockscreen show notifications.',
            'lockscreen_show_private': 'Lockscreen show private.',
            'pinned_hun_keys': 'Pinned hun keys.',
            'guts_key': 'Guts key.',
            'remote_input_keys': 'Remote input keys.',
            'user_expanded_keys': 'User expanded keys.',
            'snooze_key': 'Snooze key.',
            'overlay_keys': 'Overlay keys.',
        }))

# Keep this list sorted.
ALL_TABLES = [
    UI_HIERARCHY_EVENT_TABLE,
    UI_HIERARCHY_NODE_TABLE,
    UI_HIERARCHY_SNAPSHOT_STATE_TABLE,
    UI_HIERARCHY_SNAPSHOT_TABLE,
    UI_HIERARCHY_SYSUI_STATE_TABLE,
    UI_HIERARCHY_WINDOW_FRAME_TABLE,
    UI_HIERARCHY_WINDOW_TABLE,
]
