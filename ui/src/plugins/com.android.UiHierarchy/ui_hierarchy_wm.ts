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

// WindowManager (android.windowmanager) data for the Screen level of the
// viewer. WM containers are mapped onto UiHierarchyNode so the hierarchy tree,
// layout canvas, search and hide/show work unchanged at both levels.

import type {Engine} from '../../trace_processor/engine';
import {
  LONG,
  NUM,
  NUM_NULL,
  STR,
  STR_NULL,
} from '../../trace_processor/query_result';
import type {UiHierarchyNode, UiHierarchyWindow} from './ui_hierarchy_data';

// Node kinds for WM containers. Disjoint from the View/Compose kinds (1-4).
export const WM_KIND_ROOT = 100;
export const WM_KIND_DISPLAY = 101;
export const WM_KIND_DISPLAY_AREA = 102;
export const WM_KIND_TASK = 103;
export const WM_KIND_TASK_FRAGMENT = 104;
export const WM_KIND_ACTIVITY = 105;
export const WM_KIND_TOKEN = 106;
export const WM_KIND_WINDOW = 107;

// Other Screen level node kinds: process groups (Processes tree) and
// SurfaceFlinger layers (SurfaceFlinger tree).
export const SCREEN_KIND_PROCESS = 110;
export const SCREEN_KIND_LAYER = 111;

const CONTAINER_KINDS: Record<string, number> = {
  RootWindowContainer: WM_KIND_ROOT,
  DisplayContent: WM_KIND_DISPLAY,
  DisplayArea: WM_KIND_DISPLAY_AREA,
  Task: WM_KIND_TASK,
  TaskFragment: WM_KIND_TASK_FRAGMENT,
  Activity: WM_KIND_ACTIVITY,
  WindowToken: WM_KIND_TOKEN,
  WindowState: WM_KIND_WINDOW,
};

export function wmKindToString(kind: number): string | undefined {
  switch (kind) {
    case WM_KIND_ROOT:
      return 'Root';
    case WM_KIND_DISPLAY:
      return 'Display';
    case WM_KIND_DISPLAY_AREA:
      return 'DisplayArea';
    case WM_KIND_TASK:
      return 'Task';
    case WM_KIND_TASK_FRAGMENT:
      return 'TaskFragment';
    case WM_KIND_ACTIVITY:
      return 'Activity';
    case WM_KIND_TOKEN:
      return 'WindowToken';
    case WM_KIND_WINDOW:
      return 'Window';
    case SCREEN_KIND_PROCESS:
      return 'Process';
    case SCREEN_KIND_LAYER:
      return 'Layer';
    default:
      return undefined;
  }
}

// WindowManager.LayoutParams.type names for the common types.
const WINDOW_TYPES: Record<number, string> = {
  1: 'BASE_APPLICATION',
  2: 'APPLICATION',
  3: 'APPLICATION_STARTING',
  4: 'DRAWN_APPLICATION',
  1000: 'APPLICATION_PANEL',
  1001: 'APPLICATION_MEDIA',
  1002: 'APPLICATION_SUB_PANEL',
  1003: 'APPLICATION_ATTACHED_DIALOG',
  2000: 'STATUS_BAR',
  2005: 'TOAST',
  2008: 'SYSTEM_DIALOG',
  2011: 'INPUT_METHOD',
  2012: 'INPUT_METHOD_DIALOG',
  2013: 'WALLPAPER',
  2015: 'SECURE_SYSTEM_OVERLAY',
  2016: 'DRAG',
  2017: 'STATUS_BAR_SUB_PANEL',
  2019: 'NAVIGATION_BAR',
  2022: 'INPUT_CONSUMER',
  2024: 'NAVIGATION_BAR_PANEL',
  2026: 'DISPLAY_OVERLAY',
  2027: 'MAGNIFICATION_OVERLAY',
  2032: 'ACCESSIBILITY_OVERLAY',
  2038: 'APPLICATION_OVERLAY',
  2040: 'NOTIFICATION_SHADE',
  2041: 'STATUS_BAR_ADDITIONAL',
};

export function windowTypeToString(type: number): string {
  const name = WINDOW_TYPES[type];
  return name !== undefined ? `${name} (${type})` : `${type}`;
}

export interface WmSnapshot {
  readonly id: number;
  readonly ts: bigint;
}

// One WM container with the few args we need, keyed by short name
// (see normalizeWmArgKey).
export interface WmContainerRow {
  readonly token: number;
  readonly parentToken?: number;
  readonly childIndex: number;
  readonly title: string;
  readonly containerType: string;
  readonly isVisible: boolean;
  readonly args: ReadonlyMap<string, number>;
}

// Maps the full proto arg key to a short name, or undefined if unused.
// Frames and bounds are nested under a container-specific prefix, e.g.
// "window.window_frames.frame.left" or
// "task.task_fragment.window_container.configuration_container.
//  full_configuration.window_configuration.bounds.right".
export function normalizeWmArgKey(key: string): string | undefined {
  const side = key.slice(key.lastIndexOf('.') + 1);
  const isSide =
    side === 'left' || side === 'top' || side === 'right' || side === 'bottom';
  if (isSide && key.includes('.window_frames.frame.')) return `frame.${side}`;
  if (
    isSide &&
    key.includes('.full_configuration.window_configuration.bounds.')
  ) {
    return `bounds.${side}`;
  }
  if (key === 'window.attributes.type') return 'type';
  if (key === 'window.attributes.alpha') return 'alpha';
  if (key.endsWith('.surface_control.layerId')) return 'layerId';
  if (key === 'display_content.id') return 'displayId';
  return undefined;
}

// Builds viewer nodes from WM containers, in depth-first order with children
// sorted by child_index. WM children are stored bottom to top, so this is
// also the paint (z) order.
export function buildWmNodes(
  rows: ReadonlyArray<WmContainerRow>,
  ts: bigint,
): UiHierarchyNode[] {
  const byToken = new Map(rows.map((r) => [r.token, r]));
  const childrenOf = new Map<number, WmContainerRow[]>();
  const roots: WmContainerRow[] = [];
  for (const r of rows) {
    if (r.parentToken !== undefined && byToken.has(r.parentToken)) {
      const arr = childrenOf.get(r.parentToken);
      if (arr) arr.push(r);
      else childrenOf.set(r.parentToken, [r]);
    } else {
      roots.push(r);
    }
  }
  for (const arr of childrenOf.values()) {
    arr.sort((a, b) => a.childIndex - b.childIndex);
  }

  const out: UiHierarchyNode[] = [];
  const visited = new Set<number>();
  const visit = (r: WmContainerRow) => {
    if (visited.has(r.token)) return; // Guards against cycles.
    visited.add(r.token);
    out.push(toNode(r, ts, out.length, byToken.has(r.parentToken ?? -1)));
    for (const c of childrenOf.get(r.token) ?? []) visit(c);
  };
  roots.sort((a, b) => a.childIndex - b.childIndex).forEach(visit);
  return out;
}

function toNode(
  r: WmContainerRow,
  ts: bigint,
  rowId: number,
  hasParent: boolean,
): UiHierarchyNode {
  // Windows have a real frame; other containers only a configuration bounds.
  // Zero-valued sides are omitted from the proto, hence the defaults.
  const hasFrame = [...r.args.keys()].some((k) => k.startsWith('frame.'));
  const p = hasFrame ? 'frame' : 'bounds';
  const side = (s: string) => r.args.get(`${p}.${s}`) ?? 0;
  const kind = CONTAINER_KINDS[r.containerType] ?? WM_KIND_DISPLAY_AREA;
  return {
    rowId,
    ts,
    dur: 0n,
    windowId: 0,
    nodeId: `${r.token}`,
    parentNodeId: hasParent ? `${r.parentToken}` : undefined,
    childIndex: r.childIndex,
    kind,
    kindName: r.containerType,
    name: r.title,
    boundsLeft: side('left'),
    boundsTop: side('top'),
    boundsRight: side('right'),
    boundsBottom: side('bottom'),
    alpha: r.args.get('alpha') ?? 1,
    flags: 0n,
    isVisible: r.isVisible,
    isClickable: false,
    isFocused: false,
    isTextRedacted: false,
    wm: {
      containerType: r.containerType,
      token: r.token,
      windowType: r.args.get('type'),
      layerId: r.args.get('layerId'),
      // Display 0 is omitted from the proto.
      displayId:
        kind === WM_KIND_DISPLAY ? (r.args.get('displayId') ?? 0) : undefined,
    },
  };
}

// The UI hierarchy window drawn by a WM window, matched by title (the data
// source uses WM naming) on the same display, if known. Ambiguous titles are
// resolved by bounds.
export function matchUiWindow(
  wmWindow: UiHierarchyNode,
  windows: ReadonlyArray<UiHierarchyWindow>,
  displayId?: number,
): UiHierarchyWindow | undefined {
  if (wmWindow.kind !== WM_KIND_WINDOW) return undefined;
  const title = wmWindow.name;
  const candidates = windows.filter(
    (w) =>
      (w.title === title || title.endsWith(`/${w.title}`)) &&
      (displayId === undefined || w.displayId < 0 || w.displayId === displayId),
  );
  if (candidates.length <= 1) return candidates[0];
  return (
    candidates.find(
      (w) =>
        w.boundsLeft === wmWindow.boundsLeft &&
        w.boundsTop === wmWindow.boundsTop &&
        w.boundsRight === wmWindow.boundsRight &&
        w.boundsBottom === wmWindow.boundsBottom,
    ) ?? candidates[0]
  );
}

// The display containing `nodeId`, if any.
export function displayOf(
  byNodeId: ReadonlyMap<string, UiHierarchyNode>,
  nodeId: string | undefined,
): UiHierarchyNode | undefined {
  const seen = new Set<string>();
  let cur = nodeId !== undefined ? byNodeId.get(nodeId) : undefined;
  while (cur !== undefined && !seen.has(cur.nodeId)) {
    if (cur.kind === WM_KIND_DISPLAY) return cur;
    seen.add(cur.nodeId);
    cur =
      cur.parentNodeId !== undefined
        ? byNodeId.get(cur.parentNodeId)
        : undefined;
  }
  return undefined;
}

// The display with the most visible windows (skips e.g. empty virtual
// displays used for screen recording).
export function defaultDisplay(
  nodes: ReadonlyArray<UiHierarchyNode>,
): UiHierarchyNode | undefined {
  const byNodeId = new Map(nodes.map((n) => [n.nodeId, n]));
  const counts = new Map<string, number>();
  for (const n of nodes) {
    if (n.kind !== WM_KIND_WINDOW || !n.isVisible) continue;
    const d = displayOf(byNodeId, n.nodeId);
    if (d !== undefined) counts.set(d.nodeId, (counts.get(d.nodeId) ?? 0) + 1);
  }
  let best: UiHierarchyNode | undefined;
  let bestCount = -1;
  for (const n of nodes) {
    if (n.kind !== WM_KIND_DISPLAY) continue;
    const c = counts.get(n.nodeId) ?? 0;
    if (c > bestCount) {
      best = n;
      bestCount = c;
    }
  }
  return best;
}

// What the Display properties section shows, from DisplayContent args.
export interface WmDisplaySummary {
  readonly width?: number;
  readonly height?: number;
  readonly dpi?: number;
  readonly focusedApp?: string;
  readonly resumedActivity?: string;
  readonly currentFocus?: string;
}

const DISPLAY_SUMMARY_ARGS: Record<keyof WmDisplaySummary, string> = {
  width: 'display_content.display_info.logical_width',
  height: 'display_content.display_info.logical_height',
  dpi: 'display_content.dpi',
  focusedApp: 'display_content.focused_app',
  resumedActivity: 'display_content.resumed_activity.title',
  currentFocus: 'display_content.current_focus_identifier.title',
};

// `args`: DisplayContent arg values by key.
export function buildDisplaySummary(
  args: ReadonlyMap<string, string>,
): WmDisplaySummary {
  const str = (k: keyof WmDisplaySummary) => args.get(DISPLAY_SUMMARY_ARGS[k]);
  const num = (k: keyof WmDisplaySummary) => {
    const v = str(k);
    return v !== undefined && v !== '' ? Number(v) : undefined;
  };
  return {
    width: num('width'),
    height: num('height'),
    dpi: num('dpi'),
    focusedApp: str('focusedApp'),
    resumedActivity: str('resumedActivity'),
    currentFocus: str('currentFocus'),
  };
}

// Display summaries of a snapshot, by display container token.
export async function queryWmDisplaySummaries(
  engine: Engine,
  snapshot: WmSnapshot,
): Promise<Map<number, WmDisplaySummary>> {
  const keys = Object.values(DISPLAY_SUMMARY_ARGS)
    .map((k) => `'${k}'`)
    .join(', ');
  const res = await engine.query(`
    INCLUDE PERFETTO MODULE android.winscope.windowmanager;
    SELECT c.token, a.key, a.display_value AS value
    FROM android_windowmanager_windowcontainer c
    JOIN args a ON a.arg_set_id = c.arg_set_id AND a.key IN (${keys})
    WHERE c.snapshot_id = ${snapshot.id}
      AND c.container_type = 'DisplayContent';
  `);
  const args = new Map<number, Map<string, string>>();
  const it = res.iter({token: NUM, key: STR, value: STR_NULL});
  for (; it.valid(); it.next()) {
    let m = args.get(it.token);
    if (m === undefined) {
      m = new Map();
      args.set(it.token, m);
    }
    if (it.value !== null) m.set(it.key, it.value);
  }
  return new Map(
    [...args].map(([token, m]) => [token, buildDisplaySummary(m)]),
  );
}

export async function queryWmSnapshots(engine: Engine): Promise<WmSnapshot[]> {
  try {
    const res = await engine.query(`
      INCLUDE PERFETTO MODULE android.winscope.windowmanager;
      SELECT id, ts FROM android_windowmanager ORDER BY ts;
    `);
    const out: WmSnapshot[] = [];
    for (const it = res.iter({id: NUM, ts: LONG}); it.valid(); it.next()) {
      out.push({id: it.id, ts: it.ts});
    }
    return out;
  } catch {
    // Traces without WM data, or trace processors without the module.
    return [];
  }
}

export async function queryWmNodes(
  engine: Engine,
  snapshot: WmSnapshot,
): Promise<UiHierarchyNode[]> {
  const res = await engine.query(`
    INCLUDE PERFETTO MODULE android.winscope.windowmanager;
    SELECT
      c.token,
      c.parent_token,
      c.child_index,
      c.title,
      c.container_type,
      c.is_visible,
      a.key,
      COALESCE(a.int_value, a.real_value) AS value
    FROM android_windowmanager_windowcontainer c
    LEFT JOIN args a ON a.arg_set_id = c.arg_set_id AND (
      a.key GLOB '*.window_frames.frame.*'
      OR a.key GLOB '*.full_configuration.window_configuration.bounds.*'
      OR a.key IN ('window.attributes.type', 'window.attributes.alpha')
      OR a.key GLOB '*.surface_control.layerId'
      OR a.key = 'display_content.id'
    )
    WHERE c.snapshot_id = ${snapshot.id};
  `);
  const rows = new Map<
    number,
    Omit<WmContainerRow, 'args'> & {args: Map<string, number>}
  >();
  const it = res.iter({
    token: NUM,
    parent_token: NUM_NULL,
    child_index: NUM_NULL,
    title: STR,
    container_type: STR,
    is_visible: NUM,
    key: STR_NULL,
    value: NUM_NULL,
  });
  for (; it.valid(); it.next()) {
    let row = rows.get(it.token);
    if (row === undefined) {
      row = {
        token: it.token,
        parentToken: it.parent_token ?? undefined,
        childIndex: it.child_index ?? 0,
        title: it.title,
        containerType: it.container_type,
        isVisible: it.is_visible !== 0,
        args: new Map(),
      };
      rows.set(it.token, row);
    }
    const key = it.key !== null ? normalizeWmArgKey(it.key) : undefined;
    if (key !== undefined && it.value !== null) row.args.set(key, it.value);
  }
  return buildWmNodes([...rows.values()], snapshot.ts);
}
