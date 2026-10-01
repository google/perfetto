// Copyright (C) 2026 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

import type {Engine} from '../../trace_processor/engine';
import {
  LONG,
  LONG_NULL,
  NUM,
  NUM_NULL,
  STR,
  STR_NULL,
} from '../../trace_processor/query_result';
import {wmKindToString} from './ui_hierarchy_wm';

export interface UiHierarchyWindow {
  readonly key: string;
  readonly upid: number;
  readonly pid?: number;
  readonly processName?: string;
  readonly windowId: number;
  readonly title: string;
  readonly displayId: number;
  readonly boundsLeft: number;
  readonly boundsTop: number;
  readonly boundsRight: number;
  readonly boundsBottom: number;
}

export interface UiHierarchySnapshot {
  readonly snapshotId: number;
  readonly ts: bigint;
  readonly isKeyframe: boolean;
  readonly vsyncId?: bigint;
  readonly textMode?: number;
  readonly changedNodeCount: number;
  readonly removedNodeCount: number;
}

export interface UiHierarchyNode {
  readonly rowId: number;
  readonly ts: bigint;
  readonly dur: bigint;
  readonly windowId: number;
  readonly nodeId: string;
  readonly parentNodeId?: string;
  readonly childIndex: number;
  readonly kind: number;
  readonly kindName: string;
  readonly name: string;
  readonly sourceLocation?: string;
  readonly boundsLeft: number;
  readonly boundsTop: number;
  readonly boundsRight: number;
  readonly boundsBottom: number;
  readonly alpha: number;
  readonly flags: bigint;
  readonly isVisible: boolean;
  readonly isClickable: boolean;
  readonly isFocused: boolean;
  readonly isTextRedacted: boolean;
  readonly text?: string;
  readonly contentDescription?: string;
  readonly testTag?: string;
  readonly role?: string;
  readonly stateDescription?: string;
  readonly actions?: string;
  readonly properties?: string;
  readonly recompositionCount?: number;
  readonly skipCount?: number;
  readonly elevation?: number;
  // Set for WindowManager containers (Screen level) only.
  readonly wm?: WmNodeInfo;
  // Set for SurfaceFlinger layers (Screen level) only.
  readonly sfLayerId?: number;
}

export interface WmNodeInfo {
  readonly containerType: string;
  readonly token: number;
  readonly windowType?: number;
  readonly layerId?: number;
  // Set on displays only.
  readonly displayId?: number;
}

export interface NodeVersionChange {
  readonly ts: bigint;
  readonly versionIndex: number;
  readonly text?: string;
  readonly bounds: string;
  readonly changedProps: string[];
}

export interface ScreenTextEntry {
  readonly role: string;
  readonly label: string;
  readonly testTag?: string;
  readonly isClickable: boolean;
  readonly boundsLeft: number;
  readonly boundsTop: number;
}

export function kindToString(kind: number): string {
  switch (kind) {
    case 1:
      return 'View';
    case 2:
      return 'ComposeView';
    case 3:
      return 'ComposeNode';
    case 4:
      return 'Composable';
    default:
      return wmKindToString(kind) ?? 'Unknown';
  }
}

export async function queryWindows(
  engine: Engine,
): Promise<UiHierarchyWindow[]> {
  const query = `
    SELECT
      w.upid,
      p.pid,
      p.name AS process_name,
      w.window_id,
      COALESCE(w.title, 'Window ' || w.window_id) AS title,
      w.display_id,
      w.bounds_left,
      w.bounds_top,
      w.bounds_right,
      w.bounds_bottom
    FROM __intrinsic_ui_hierarchy_window w
    LEFT JOIN process p ON w.upid = p.upid
    GROUP BY w.upid, w.window_id
    ORDER BY w.upid, w.window_id;
  `;
  const res = await engine.query(query);
  const it = res.iter({
    upid: NUM_NULL,
    pid: NUM_NULL,
    process_name: STR_NULL,
    window_id: NUM,
    title: STR,
    display_id: NUM,
    bounds_left: NUM,
    bounds_top: NUM,
    bounds_right: NUM,
    bounds_bottom: NUM,
  });
  const out: UiHierarchyWindow[] = [];
  for (; it.valid(); it.next()) {
    const upid = it.upid ?? 0;
    out.push({
      key: `${upid}:${it.window_id}`,
      upid,
      pid: it.pid !== null ? it.pid : undefined,
      processName: it.process_name !== null ? it.process_name : undefined,
      windowId: it.window_id,
      title: it.title,
      displayId: it.display_id,
      boundsLeft: it.bounds_left,
      boundsTop: it.bounds_top,
      boundsRight: it.bounds_right,
      boundsBottom: it.bounds_bottom,
    });
  }
  return out;
}

// The id of the window's frame shown at `ts` (the latest frame at or before
// it), which is the event id on the window's timeline track. Snapshot ids are
// per process and can't be used for that.
export async function queryWindowFrameIdAt(
  engine: Engine,
  upid: number,
  windowId: number,
  ts: bigint,
): Promise<number | undefined> {
  const res = await engine.query(`
    SELECT id
    FROM __intrinsic_ui_hierarchy_window_frame
    WHERE upid = ${upid} AND window_id = ${windowId} AND NOT is_removed
    ORDER BY ts > ${ts}, ABS(ts - ${ts})
    LIMIT 1;
  `);
  const it = res.iter({id: NUM});
  return it.valid() ? it.id : undefined;
}

// The snapshots of process `upid`, or with `windowId` only those carrying a
// frame of that window (a process emits one snapshot per window per frame).
export async function querySnapshots(
  engine: Engine,
  upid?: number,
  windowId?: number,
): Promise<UiHierarchySnapshot[]> {
  const conds: string[] = [];
  if (upid !== undefined) conds.push(`upid = ${upid}`);
  if (windowId !== undefined) {
    conds.push(`id IN (
      SELECT snapshot_id
      FROM __intrinsic_ui_hierarchy_window_frame
      WHERE window_id = ${windowId} AND NOT is_removed
    )`);
  }
  const whereClause = conds.length > 0 ? `WHERE ${conds.join(' AND ')}` : '';
  const query = `
    SELECT
      id,
      ts,
      is_keyframe,
      vsync_id,
      text_mode,
      changed_node_count,
      removed_node_count
    FROM __intrinsic_ui_hierarchy_snapshot
    ${whereClause}
    ORDER BY ts;
  `;
  const res = await engine.query(query);
  const it = res.iter({
    id: NUM,
    ts: LONG,
    is_keyframe: NUM,
    vsync_id: LONG_NULL,
    text_mode: NUM_NULL,
    changed_node_count: NUM,
    removed_node_count: NUM,
  });
  const out: UiHierarchySnapshot[] = [];
  for (; it.valid(); it.next()) {
    out.push({
      snapshotId: it.id,
      ts: it.ts,
      isKeyframe: it.is_keyframe !== 0,
      vsyncId: it.vsync_id !== null ? it.vsync_id : undefined,
      textMode: it.text_mode !== null ? it.text_mode : undefined,
      changedNodeCount: it.changed_node_count,
      removedNodeCount: it.removed_node_count,
    });
  }
  return out;
}

export async function queryNodesAt(
  engine: Engine,
  windowId: number,
  ts: bigint,
): Promise<UiHierarchyNode[]> {
  const query = `
    WITH active_nodes AS (
      SELECT
        *,
        ROW_NUMBER() OVER (PARTITION BY node_id ORDER BY ts DESC) AS rn
      FROM __intrinsic_ui_hierarchy_node
      WHERE window_id = ${windowId}
        AND ts <= ${ts}
        AND (dur = -1 OR dur = 0 OR ts + dur >= ${ts})
    )
    SELECT
      id,
      ts,
      dur,
      window_id,
      CAST(node_id AS TEXT) AS node_id,
      CAST(parent_node_id AS TEXT) AS parent_node_id,
      child_index,
      kind,
      COALESCE(name, 'Node ' || node_id) AS name,
      source_location,
      bounds_left,
      bounds_top,
      bounds_right,
      bounds_bottom,
      alpha,
      flags,
      (flags & 1) != 0 AS is_visible,
      (flags & 4) != 0 AS is_clickable,
      (flags & 32) != 0 AS is_focused,
      (flags & 131072) != 0 AS is_text_redacted,
      text,
      content_description,
      test_tag,
      role,
      state_description,
      actions,
      properties,
      NULL AS recomposition_count,
      NULL AS skip_count,
      elevation
    FROM active_nodes
    WHERE rn = 1
    ORDER BY parent_node_id NULLS FIRST, child_index;
  `;
  const res = await engine.query(query);
  const it = res.iter({
    id: NUM,
    ts: LONG,
    dur: LONG,
    window_id: NUM,
    node_id: STR,
    parent_node_id: STR_NULL,
    child_index: NUM,
    kind: NUM,
    name: STR,
    source_location: STR_NULL,
    bounds_left: NUM,
    bounds_top: NUM,
    bounds_right: NUM,
    bounds_bottom: NUM,
    alpha: NUM,
    flags: LONG,
    is_visible: NUM,
    is_clickable: NUM,
    is_focused: NUM,
    is_text_redacted: NUM,
    text: STR_NULL,
    content_description: STR_NULL,
    test_tag: STR_NULL,
    role: STR_NULL,
    state_description: STR_NULL,
    actions: STR_NULL,
    properties: STR_NULL,
    recomposition_count: NUM_NULL,
    skip_count: NUM_NULL,
    elevation: NUM_NULL,
  });
  const out: UiHierarchyNode[] = [];
  for (; it.valid(); it.next()) {
    out.push({
      rowId: it.id,
      ts: it.ts,
      dur: it.dur,
      windowId: it.window_id,
      nodeId: it.node_id,
      parentNodeId: it.parent_node_id !== null ? it.parent_node_id : undefined,
      childIndex: it.child_index,
      kind: it.kind,
      kindName: kindToString(it.kind),
      name: it.name,
      sourceLocation:
        it.source_location !== null ? it.source_location : undefined,
      boundsLeft: it.bounds_left,
      boundsTop: it.bounds_top,
      boundsRight: it.bounds_right,
      boundsBottom: it.bounds_bottom,
      alpha: it.alpha,
      flags: it.flags,
      isVisible: it.is_visible !== 0,
      isClickable: it.is_clickable !== 0,
      isFocused: it.is_focused !== 0,
      isTextRedacted: it.is_text_redacted !== 0,
      text: it.text !== null ? it.text : undefined,
      contentDescription:
        it.content_description !== null ? it.content_description : undefined,
      testTag: it.test_tag !== null ? it.test_tag : undefined,
      role: it.role !== null ? it.role : undefined,
      stateDescription:
        it.state_description !== null ? it.state_description : undefined,
      actions: it.actions !== null ? it.actions : undefined,
      properties: it.properties !== null ? it.properties : undefined,
      recompositionCount:
        it.recomposition_count !== null ? it.recomposition_count : undefined,
      skipCount: it.skip_count !== null ? it.skip_count : undefined,
      elevation: it.elevation !== null ? it.elevation : undefined,
    });
  }
  return out;
}

export async function queryNodeVersions(
  engine: Engine,
  windowId: number,
  nodeId: string,
): Promise<UiHierarchyNode[]> {
  const query = `
    SELECT
      id,
      ts,
      dur,
      window_id,
      CAST(node_id AS TEXT) AS node_id,
      CAST(parent_node_id AS TEXT) AS parent_node_id,
      child_index,
      kind,
      COALESCE(name, 'Node ' || node_id) AS name,
      source_location,
      bounds_left,
      bounds_top,
      bounds_right,
      bounds_bottom,
      alpha,
      flags,
      (flags & 1) != 0 AS is_visible,
      (flags & 4) != 0 AS is_clickable,
      (flags & 32) != 0 AS is_focused,
      (flags & 131072) != 0 AS is_text_redacted,
      text,
      content_description,
      test_tag,
      role,
      state_description,
      actions,
      properties,
      NULL AS recomposition_count,
      NULL AS skip_count,
      elevation
    FROM __intrinsic_ui_hierarchy_node
    WHERE window_id = ${windowId} AND node_id = ${nodeId}
    ORDER BY ts;
  `;
  const res = await engine.query(query);
  const it = res.iter({
    id: NUM,
    ts: LONG,
    dur: LONG,
    window_id: NUM,
    node_id: STR,
    parent_node_id: STR_NULL,
    child_index: NUM,
    kind: NUM,
    name: STR,
    source_location: STR_NULL,
    bounds_left: NUM,
    bounds_top: NUM,
    bounds_right: NUM,
    bounds_bottom: NUM,
    alpha: NUM,
    flags: LONG,
    is_visible: NUM,
    is_clickable: NUM,
    is_focused: NUM,
    is_text_redacted: NUM,
    text: STR_NULL,
    content_description: STR_NULL,
    test_tag: STR_NULL,
    role: STR_NULL,
    state_description: STR_NULL,
    actions: STR_NULL,
    properties: STR_NULL,
    recomposition_count: NUM_NULL,
    skip_count: NUM_NULL,
    elevation: NUM_NULL,
  });
  const out: UiHierarchyNode[] = [];
  for (; it.valid(); it.next()) {
    out.push({
      rowId: it.id,
      ts: it.ts,
      dur: it.dur,
      windowId: it.window_id,
      nodeId: it.node_id,
      parentNodeId: it.parent_node_id !== null ? it.parent_node_id : undefined,
      childIndex: it.child_index,
      kind: it.kind,
      kindName: kindToString(it.kind),
      name: it.name,
      sourceLocation:
        it.source_location !== null ? it.source_location : undefined,
      boundsLeft: it.bounds_left,
      boundsTop: it.bounds_top,
      boundsRight: it.bounds_right,
      boundsBottom: it.bounds_bottom,
      alpha: it.alpha,
      flags: it.flags,
      isVisible: it.is_visible !== 0,
      isClickable: it.is_clickable !== 0,
      isFocused: it.is_focused !== 0,
      isTextRedacted: it.is_text_redacted !== 0,
      text: it.text !== null ? it.text : undefined,
      contentDescription:
        it.content_description !== null ? it.content_description : undefined,
      testTag: it.test_tag !== null ? it.test_tag : undefined,
      role: it.role !== null ? it.role : undefined,
      stateDescription:
        it.state_description !== null ? it.state_description : undefined,
      actions: it.actions !== null ? it.actions : undefined,
      properties: it.properties !== null ? it.properties : undefined,
      recompositionCount:
        it.recomposition_count !== null ? it.recomposition_count : undefined,
      skipCount: it.skip_count !== null ? it.skip_count : undefined,
      elevation: it.elevation !== null ? it.elevation : undefined,
    });
  }
  return out;
}

export function computeVersionDiffs(
  versions: UiHierarchyNode[],
): NodeVersionChange[] {
  const out: NodeVersionChange[] = [];
  for (let i = 0; i < versions.length; i++) {
    const cur = versions[i];
    const prev = i > 0 ? versions[i - 1] : undefined;
    const changedProps: string[] = [];
    if (!prev) {
      changedProps.push('Initial version');
    } else {
      if (cur.text !== prev.text) {
        changedProps.push(`text: "${prev.text ?? ''}" → "${cur.text ?? ''}"`);
      }
      if (
        cur.boundsLeft !== prev.boundsLeft ||
        cur.boundsTop !== prev.boundsTop ||
        cur.boundsRight !== prev.boundsRight ||
        cur.boundsBottom !== prev.boundsBottom
      ) {
        changedProps.push(
          `bounds: [${prev.boundsLeft},${prev.boundsTop},${prev.boundsRight},${prev.boundsBottom}] → [${cur.boundsLeft},${cur.boundsTop},${cur.boundsRight},${cur.boundsBottom}]`,
        );
      }
      if (cur.isVisible !== prev.isVisible) {
        changedProps.push(`visible: ${prev.isVisible} → ${cur.isVisible}`);
      }
      if (cur.isClickable !== prev.isClickable) {
        changedProps.push(
          `clickable: ${prev.isClickable} → ${cur.isClickable}`,
        );
      }
      if (cur.recompositionCount !== prev.recompositionCount) {
        changedProps.push(
          `recompositions: ${prev.recompositionCount ?? 0} → ${cur.recompositionCount ?? 0}`,
        );
      }
      if (cur.role !== prev.role) {
        changedProps.push(`role: "${prev.role ?? ''}" → "${cur.role ?? ''}"`);
      }
      if (cur.flags !== prev.flags) {
        changedProps.push(`flags: ${prev.flags} → ${cur.flags}`);
      }
      if (changedProps.length === 0) {
        changedProps.push('Properties updated');
      }
    }
    out.push({
      ts: cur.ts,
      versionIndex: i,
      text: cur.text,
      bounds: `[${cur.boundsLeft}, ${cur.boundsTop}, ${cur.boundsRight}, ${cur.boundsBottom}]`,
      changedProps,
    });
  }
  return out;
}

export async function queryScreenText(
  engine: Engine,
  ts: bigint,
  upid?: number,
): Promise<ScreenTextEntry[]> {
  const whereClause = upid !== undefined ? `WHERE upid = ${upid}` : '';
  const query = `
    INCLUDE PERFETTO MODULE android.ui_hierarchy;
    SELECT
      role,
      label,
      test_tag,
      is_clickable,
      bounds_left,
      bounds_top
    FROM android_ui_hierarchy_screen_text(${ts})
    ${whereClause}
    LIMIT 30;
  `;
  const res = await engine.query(query);
  const it = res.iter({
    role: STR,
    label: STR_NULL,
    test_tag: STR_NULL,
    is_clickable: NUM,
    bounds_left: NUM,
    bounds_top: NUM,
  });
  const out: ScreenTextEntry[] = [];
  for (; it.valid(); it.next()) {
    out.push({
      role: it.role,
      label: it.label !== null ? it.label : '',
      testTag: it.test_tag !== null ? it.test_tag : undefined,
      isClickable: it.is_clickable !== 0,
      boundsLeft: it.bounds_left,
      boundsTop: it.bounds_top,
    });
  }
  return out;
}
