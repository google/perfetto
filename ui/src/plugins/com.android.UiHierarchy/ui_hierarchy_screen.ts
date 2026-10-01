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

// Screen level model: which process owns each WM window, and the
// Process › Window tree built from it.

import type {Engine} from '../../trace_processor/engine';
import {NUM, NUM_NULL, STR_NULL} from '../../trace_processor/query_result';
import type {UiHierarchyNode, UiHierarchyWindow} from './ui_hierarchy_data';
import {
  displayName,
  formatBounds,
  formatNumber,
  nodeBounds,
  type PropRow,
  PropRowsBuilder,
} from './ui_hierarchy_props';
import {type SfCompositionCounts, SYSTEM_UID} from './ui_hierarchy_sf';
import {
  SCREEN_KIND_PROCESS,
  WM_KIND_WINDOW,
  windowTypeToString,
  type WmDisplaySummary,
} from './ui_hierarchy_wm';

export interface ProcessRow {
  readonly upid: number;
  readonly pid?: number;
  readonly name: string;
  readonly uid?: number;
}

// How a window's owner was found, most to least reliable.
export type OwnerSource =
  // The window has android.ui.hierarchy data, recorded in-process.
  | 'ui_hierarchy'
  // The uid of the window's SF buffer layer.
  | 'layer_uid'
  // The package in the window title ("com.example/.MainActivity").
  | 'package'
  // No app signal: a system_server window.
  | 'system';

export interface WindowOwner {
  // Groups windows of the same process.
  readonly key: string;
  readonly name: string;
  readonly pid?: number;
  readonly upid?: number;
  readonly uid?: number;
  readonly source: OwnerSource;
}

const SYSTEM_SERVER = 'system_server';

export function ownerLabel(o: WindowOwner): string {
  return o.pid !== undefined ? `${o.name} (${o.pid})` : o.name;
}

export function ownerSourceDescription(source: OwnerSource): string {
  switch (source) {
    case 'ui_hierarchy':
      return 'UI hierarchy data recorded by the process';
    case 'layer_uid':
      return 'Owner uid of the window\u2019s SurfaceFlinger buffer layer';
    case 'package':
      return 'Package name in the window title';
    case 'system':
      return 'No app layer or package: assumed to be a system_server window';
  }
}

// The package of an activity window title ("com.example/.MainActivity" or
// "com.example/com.example.MainActivity"), if any.
export function packageOfTitle(title: string): string | undefined {
  const m = /^([a-zA-Z]\w*(?:\.\w+)+)\//.exec(title);
  return m !== null ? m[1] : undefined;
}

function ownerFromProcess(p: ProcessRow, source: OwnerSource): WindowOwner {
  return {
    key: `upid:${p.upid}`,
    name: p.name,
    pid: p.pid,
    upid: p.upid,
    uid: p.uid,
    source,
  };
}

// The main process among `candidates` (processes of one uid): the one named
// like the package, else the shortest name without a ":service" suffix
// (shared uids run several packages), else the latest process.
function pickProcess(
  candidates: ReadonlyArray<ProcessRow>,
  pkg: string | undefined,
): ProcessRow | undefined {
  const byUpidDesc = [...candidates].sort((a, b) => b.upid - a.upid);
  const main = byUpidDesc
    .filter((p) => !p.name.includes(':'))
    .sort((a, b) => a.name.length - b.name.length);
  return (
    byUpidDesc.find((p) => pkg !== undefined && p.name === pkg) ??
    main[0] ??
    byUpidDesc[0]
  );
}

export interface WindowOwnerSignals {
  readonly title: string;
  // The android.ui.hierarchy window drawn by this WM window, if any.
  readonly uiWindow?: UiHierarchyWindow;
  // The non-system uid of the window's SF layers, if any.
  readonly appUid?: number;
}

// The process owning a window. First hit wins: in-process UI hierarchy data,
// the app uid of its SF layers, the package in its title, else
// system_server.
export function resolveWindowOwner(
  w: WindowOwnerSignals,
  processes: ReadonlyArray<ProcessRow>,
): WindowOwner {
  if (w.uiWindow !== undefined) {
    const u = w.uiWindow;
    return {
      key: `upid:${u.upid}`,
      name: u.processName ?? `Process ${u.pid ?? u.upid}`,
      pid: u.pid,
      upid: u.upid,
      source: 'ui_hierarchy',
    };
  }
  const pkg = packageOfTitle(w.title);
  if (w.appUid !== undefined && w.appUid !== SYSTEM_UID) {
    const uid = w.appUid;
    const p = pickProcess(
      processes.filter((p) => p.uid === uid),
      pkg,
    );
    if (p !== undefined) return ownerFromProcess(p, 'layer_uid');
    return {key: `uid:${uid}`, name: `uid ${uid}`, uid, source: 'layer_uid'};
  }
  if (pkg !== undefined) {
    const p = processes.find((p) => p.name === pkg);
    if (p !== undefined) return ownerFromProcess(p, 'package');
    return {key: `package:${pkg}`, name: pkg, source: 'package'};
  }
  const ss = processes.find((p) => p.name === SYSTEM_SERVER);
  if (ss !== undefined) return ownerFromProcess(ss, 'system');
  return {
    key: 'system',
    name: SYSTEM_SERVER,
    uid: SYSTEM_UID,
    source: 'system',
  };
}

export interface ProcessGroup {
  readonly owner: WindowOwner;
  // Top of the z-order first.
  readonly windows: ReadonlyArray<UiHierarchyNode>;
}

// Groups windows (given back to front) by owner. Groups are ordered by their
// topmost window, windows top first.
export function groupWindowsByProcess(
  windows: ReadonlyArray<UiHierarchyNode>,
  ownerOf: (w: UiHierarchyNode) => WindowOwner,
): ProcessGroup[] {
  const groups = new Map<
    string,
    {owner: WindowOwner; windows: UiHierarchyNode[]}
  >();
  for (const w of [...windows].reverse()) {
    const owner = ownerOf(w);
    const g = groups.get(owner.key);
    if (g !== undefined) g.windows.push(w);
    else groups.set(owner.key, {owner, windows: [w]});
  }
  return [...groups.values()];
}

export function processNodeId(owner: WindowOwner): string {
  return `process:${owner.key}`;
}

// Tree nodes for the Processes mode: a node per process with its windows
// (copies of the WM window nodes, reparented) as children.
export function buildProcessTreeNodes(
  groups: ReadonlyArray<ProcessGroup>,
  ts: bigint,
): UiHierarchyNode[] {
  const out: UiHierarchyNode[] = [];
  groups.forEach((g, gi) => {
    const id = processNodeId(g.owner);
    out.push({
      rowId: out.length,
      ts,
      dur: 0n,
      windowId: 0,
      nodeId: id,
      childIndex: gi,
      kind: SCREEN_KIND_PROCESS,
      kindName: 'Process',
      name: ownerLabel(g.owner),
      boundsLeft: 0,
      boundsTop: 0,
      boundsRight: 0,
      boundsBottom: 0,
      alpha: 1,
      flags: 0n,
      isVisible: g.windows.some((w) => w.isVisible),
      isClickable: false,
      isFocused: false,
      isTextRedacted: false,
    });
    g.windows.forEach((w, wi) => {
      out.push({...w, rowId: out.length, parentNodeId: id, childIndex: wi});
    });
  });
  return out;
}

// The processes that can own the given windows: those with the given uids
// or names, plus system_server.
export async function queryProcesses(
  engine: Engine,
  uids: ReadonlyArray<number>,
  names: ReadonlyArray<string>,
): Promise<ProcessRow[]> {
  const uidList = uids.length > 0 ? uids.join(', ') : 'NULL';
  const nameList = [...names, SYSTEM_SERVER]
    .map((n) => `'${n.replace(/'/g, "''")}'`)
    .join(', ');
  const res = await engine.query(`
    SELECT upid, pid, name, uid
    FROM process
    WHERE name IS NOT NULL AND (uid IN (${uidList}) OR name IN (${nameList}));
  `);
  const out: ProcessRow[] = [];
  const it = res.iter({
    upid: NUM,
    pid: NUM_NULL,
    name: STR_NULL,
    uid: NUM_NULL,
  });
  for (; it.valid(); it.next()) {
    out.push({
      upid: it.upid,
      pid: it.pid ?? undefined,
      name: it.name ?? '',
      uid: it.uid ?? undefined,
    });
  }
  return out;
}

// The properties of a WM container. Windows link to their process.
export function wmNodeRows(
  n: UiHierarchyNode,
  owner: WindowOwner | undefined,
): PropRow[] {
  const wm = n.wm;
  const isWindow = n.kind === WM_KIND_WINDOW;
  const b = new PropRowsBuilder()
    .text(isWindow ? 'Frame' : 'Bounds', formatBounds(nodeBounds(n)), {
      mono: true,
    })
    .text('Visible', String(n.isVisible));
  if (wm?.windowType !== undefined) {
    b.text('Type', windowTypeToString(wm.windowType), {mono: true});
  }
  if (n.alpha !== 1) b.text('Alpha', formatNumber(n.alpha));
  if (wm?.layerId !== undefined) b.text('Layer id', `${wm.layerId}`);
  if (wm !== undefined) b.copy('Token', `0x${wm.token.toString(16)}`);
  if (owner !== undefined) {
    b.links('Process', [
      {
        text: ownerLabel(owner),
        title: ownerSourceDescription(owner.source),
        target: {kind: 'process', key: owner.key},
      },
    ]);
  }
  return b.build();
}

// The properties of a process row: its uid, how its windows were
// attributed to it, and links to them.
export function processRows(group: ProcessGroup): PropRow[] {
  const o = group.owner;
  return new PropRowsBuilder()
    .text('UID', o.uid !== undefined ? `${o.uid}` : undefined)
    .text('Attribution', ownerSourceDescription(o.source))
    .links(
      'Windows',
      group.windows.map((w) => ({
        text: displayName(w.name),
        title: w.name,
        target: {kind: 'window', nodeId: w.nodeId},
      })),
    )
    .build();
}

// The properties of a display: its WM summary and, with SurfaceFlinger
// data, how many of its layers are visible and how they were composed.
export function displayRows(
  summary: WmDisplaySummary | undefined,
  layers: SfCompositionCounts | undefined,
): PropRow[] {
  const b = new PropRowsBuilder();
  const short = (label: string, name: string | undefined) =>
    b.text(label, name !== undefined ? displayName(name) : undefined, {
      title: name,
    });
  if (summary?.width !== undefined && summary.height !== undefined) {
    b.text('Size', `${summary.width} x ${summary.height} px`);
  }
  if (summary?.dpi !== undefined) b.text('Density', `${summary.dpi} dpi`);
  short('Focused app', summary?.focusedApp);
  short('Resumed activity', summary?.resumedActivity);
  short('Current focus', summary?.currentFocus);
  if (layers !== undefined) {
    b.text(
      'Visible layers',
      `${layers.visible} (${layers.hwc} HWC, ${layers.gpu} GPU)`,
    );
  }
  return b.build();
}
