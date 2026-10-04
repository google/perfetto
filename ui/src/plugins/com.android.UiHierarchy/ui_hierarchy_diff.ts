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

// Differences between two snapshots of a hierarchy tree, the same way on
// both levels: nodes are matched by node id and compared through the
// property sections the Properties pane shows, so a node is "changed"
// exactly when its properties show an old -> new value.

import type {UiHierarchyNode} from './ui_hierarchy_data';
import type {PropRow, PropSection} from './ui_hierarchy_props';

export type NodeChange = 'added' | 'changed' | 'removed';

export interface TreeDiff {
  // Added and changed nodes of the current tree, by node id.
  readonly changes: ReadonlyMap<string, NodeChange>;
  // The roots of the removed subtrees (nodes of the previous tree).
  readonly removed: ReadonlyArray<UiHierarchyNode>;
}

export const NO_DIFF: TreeDiff = {changes: new Map(), removed: []};

// Rows that change on (nearly) every snapshot without the node changing:
// the SurfaceFlinger frame number moves on each redraw, and the Compose
// recomposition/skip counts on each recomposition of a parent. They are
// shown but never marked, so they don't make a node "changed". `section`
// undefined matches the row in any section.
export const VOLATILE_ROWS: ReadonlyArray<{
  readonly section?: string;
  readonly label: string;
}> = [
  {section: 'SurfaceFlinger', label: 'Frame'},
  {label: 'Recompositions'},
  // Relative to the snapshot time.
  {section: 'Transition', label: 'Started'},
];

function isVolatile(section: string, label: string): boolean {
  return VOLATILE_ROWS.some(
    (v) => v.label === label && (v.section ?? section) === section,
  );
}

// Marks the rows of `cur` that differ from `prev` and appends the rows that
// are gone, after the row they followed.
export function diffSections(
  prev: ReadonlyArray<PropSection>,
  cur: ReadonlyArray<PropSection>,
): PropSection[] {
  const out = cur.map((section) => {
    const before = prev.find((p) => p.title === section.title);
    return {
      title: section.title,
      rows: diffRows(section.title, before?.rows ?? [], section.rows),
    };
  });
  for (const p of prev) {
    if (cur.some((c) => c.title === p.title)) continue;
    out.push({title: p.title, rows: diffRows(p.title, p.rows, [])});
  }
  return out;
}

function diffRows(
  section: string,
  prev: ReadonlyArray<PropRow>,
  cur: ReadonlyArray<PropRow>,
): PropRow[] {
  const out: PropRow[] = cur.map((row) => {
    if (isVolatile(section, row.label)) return row;
    const before = prev.find((p) => p.label === row.label);
    if (before === undefined) return {...row, diff: {kind: 'added'}};
    return sameValue(before, row)
      ? row
      : {...row, diff: {kind: 'changed', previous: before.value}};
  });
  prev.forEach((row, i) => {
    if (isVolatile(section, row.label)) return;
    if (cur.some((c) => c.label === row.label)) return;
    // After the closest earlier row that is still there, else first.
    let at = 0;
    for (let j = i - 1; j >= 0; j--) {
      const k = out.findIndex((o) => o.label === prev[j].label);
      if (k >= 0) {
        at = k + 1;
        break;
      }
    }
    out.splice(at, 0, {...row, diff: {kind: 'removed'}});
  });
  return out;
}

function sameValue(a: PropRow, b: PropRow): boolean {
  return JSON.stringify(a.value) === JSON.stringify(b.value);
}

// The snapshot to compare snapshot `current` with: `base` if it comes
// before it, else the previous snapshot (none for the first one).
export function diffBaseIndex(
  current: number,
  base: number | undefined,
): number | undefined {
  if (base !== undefined && base < current) return base;
  return current > 0 ? current - 1 : undefined;
}

export function hasDiff(sections: ReadonlyArray<PropSection>): boolean {
  return sections.some((s) => s.rows.some((r) => r.diff !== undefined));
}

// Compares two trees: nodes only in `cur` are added, nodes in both whose
// sections differ are changed, and the top-most nodes only in `prev` are
// removed. `sectionsOf` gives the properties of a node of either tree.
export function diffTrees(
  prev: ReadonlyArray<UiHierarchyNode>,
  cur: ReadonlyArray<UiHierarchyNode>,
  sectionsOf: (n: UiHierarchyNode, side: 'prev' | 'cur') => PropSection[],
): TreeDiff {
  const prevById = new Map(prev.map((n) => [n.nodeId, n]));
  const curIds = new Set(cur.map((n) => n.nodeId));
  const changes = new Map<string, NodeChange>();
  for (const n of cur) {
    const before = prevById.get(n.nodeId);
    if (before === undefined) {
      changes.set(n.nodeId, 'added');
    } else if (
      hasDiff(diffSections(sectionsOf(before, 'prev'), sectionsOf(n, 'cur')))
    ) {
      changes.set(n.nodeId, 'changed');
    }
  }
  const removed = prev.filter(
    (n) =>
      !curIds.has(n.nodeId) &&
      (n.parentNodeId === undefined ||
        curIds.has(n.parentNodeId) ||
        !prevById.has(n.parentNodeId)),
  );
  return {changes, removed};
}

// The ids of the changed, added and removed nodes and their ancestors
// (looked up in `byNodeId`, then among the removed nodes).
export function changedWithAncestors(
  diff: TreeDiff,
  byNodeId: ReadonlyMap<string, UiHierarchyNode>,
): Set<string> {
  const removedById = new Map(diff.removed.map((n) => [n.nodeId, n]));
  const keep = new Set<string>();
  const ids = [...diff.changes.keys(), ...removedById.keys()];
  for (const id of ids) {
    for (
      let cur = byNodeId.get(id) ?? removedById.get(id);
      cur !== undefined && !keep.has(cur.nodeId);
      cur =
        cur.parentNodeId !== undefined
          ? byNodeId.get(cur.parentNodeId)
          : undefined
    ) {
      keep.add(cur.nodeId);
    }
  }
  return keep;
}
