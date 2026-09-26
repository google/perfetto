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

// Diffing of tree explorer trees: pairs up the nodes of a current and a
// baseline tree (both fetched with the same metric, filters and view), lays
// the union out as a single flamegraph, and scores and colors every node by
// how it changed, pprof-style.

import {assertUnreachable} from '../base/assert';
import {type Color, HSLColor, rgbToHsl} from '../base/color';
import {
  type TreeExplorerData,
  type TreeExplorerDiffColor,
  type TreeExplorerDiffWidth,
  type TreeExplorerNode,
  type TreeExplorerPropertyDefinition,
  displaySize,
} from './tree_explorer';

// A node of the union of the two trees: the node of either tree, or both
// paired up, at one position in the tree.
interface PairedNode {
  // Unique among the paired nodes; keys their children.
  readonly index: number;
  readonly parent?: PairedNode;
  readonly depth: number;
  readonly children: PairedNode[];
  // The first node of each tree which landed here (there is at most one
  // unless a tree has siblings its pairing key cannot tell apart).
  current?: TreeExplorerNode;
  baseline?: TreeExplorerNode;
  currentSelf: number;
  currentCumulative: number;
  baselineSelf: number;
  baselineCumulative: number;
  width: number;
  x: number;
  // Position in the output.
  id: number;
}

// Builds the diff of `current` against `baseline`: the union of their nodes,
// where a node of one tree is paired with the node of the other tree at the
// same position, i.e. with the same name and grouping (unaggregatable)
// properties, under paired parents. Profile-specific properties (see
// TreeExplorerPropertyDefinition) are not compared.
//
// Nodes carry the current values in selfValue/cumulativeValue and the
// baseline ones in baselineSelfValue/baselineCumulativeValue, with 0 for the
// tree a node is missing from. They are laid out like the flamegraph operator
// does (widest siblings first, one strip per direction, parents before
// children) with widths per `width`, so nodes of zero width are laid out but
// take no space.
//
// Both inputs must list parents before children, as TreeExplorerFetcher
// does. The result takes its totals and actions from `current`.
export function buildTreeExplorerDiff(
  current: TreeExplorerData,
  baseline: TreeExplorerData,
  width: TreeExplorerDiffWidth,
): TreeExplorerData {
  // Paired nodes by parent index and pairing key. Roots are keyed under a
  // virtual parent per direction: in a PIVOT view both strips have the same
  // roots.
  const pairedByKey = new Map<string, PairedNode>();
  const downwardRoots: PairedNode[] = [];
  const upwardRoots: PairedNode[] = [];

  const pairInto = (data: TreeExplorerData, isCurrent: boolean) => {
    const pairedNodes = new Map<number, PairedNode>();
    for (const node of data.nodes) {
      const parent =
        node.parentId === -1 ? undefined : pairedNodes.get(node.parentId);
      if (node.parentId !== -1 && parent === undefined) {
        // Only reachable if parents are not listed first.
        continue;
      }
      const parentIndex = parent?.index ?? (node.depth < 0 ? -2 : -1);
      const key = `${parentIndex}\u0000${pairingKey(node)}`;
      let paired = pairedByKey.get(key);
      if (paired === undefined) {
        paired = {
          index: pairedByKey.size,
          parent,
          depth: node.depth,
          children: [],
          currentSelf: 0,
          currentCumulative: 0,
          baselineSelf: 0,
          baselineCumulative: 0,
          width: 0,
          x: 0,
          id: -1,
        };
        pairedByKey.set(key, paired);
        if (parent !== undefined) {
          parent.children.push(paired);
        } else if (node.depth < 0) {
          upwardRoots.push(paired);
        } else {
          downwardRoots.push(paired);
        }
      }
      if (isCurrent) {
        paired.current ??= node;
        paired.currentSelf += node.selfValue;
        paired.currentCumulative += node.cumulativeValue;
      } else {
        paired.baseline ??= node;
        paired.baselineSelf += node.selfValue;
        paired.baselineCumulative += node.cumulativeValue;
      }
      pairedNodes.set(node.id, paired);
    }
  };
  pairInto(current, true);
  pairInto(baseline, false);

  for (const paired of pairedByKey.values()) {
    paired.width = diffWidth(
      paired.baselineCumulative,
      paired.currentCumulative,
      width,
    );
  }

  // Lay out like the operator's ComputeLayout: each strip starts at x = 0,
  // siblings are packed widest first (ties in pairing order, i.e. the
  // current tree's order, then the baseline-only nodes) and the breadth-first
  // walk order, with the strips' roots merged by x, is the output order.
  const byWidthDesc = (a: PairedNode, b: PairedNode) => b.width - a.width;
  const layoutStrip = (roots: PairedNode[]) => {
    roots.sort(byWidthDesc);
    let x = 0;
    for (const root of roots) {
      root.x = x;
      x += root.width;
    }
    return x;
  };
  const layoutWidth = Math.max(
    layoutStrip(downwardRoots),
    layoutStrip(upwardRoots),
  );
  const order: PairedNode[] = [];
  const emit = (paired: PairedNode) => {
    paired.id = order.length;
    order.push(paired);
  };
  for (let d = 0, u = 0; d < downwardRoots.length || u < upwardRoots.length;) {
    if (
      u >= upwardRoots.length ||
      (d < downwardRoots.length && downwardRoots[d].x <= upwardRoots[u].x)
    ) {
      emit(downwardRoots[d++]);
    } else {
      emit(upwardRoots[u++]);
    }
  }
  for (let i = 0; i < order.length; i++) {
    const paired = order[i];
    paired.children.sort(byWidthDesc);
    let x = paired.x;
    for (const child of paired.children) {
      child.x = x;
      x += child.width;
      emit(child);
    }
  }

  let minDepth = 0;
  let maxDepth = 0;
  let maxAbsDelta = Math.abs(
    current.allRootsCumulativeValue - baseline.allRootsCumulativeValue,
  );
  const nodes: TreeExplorerNode[] = order.map((paired) => {
    // Every paired node holds a node of at least one of the trees.
    const source = (paired.current ?? paired.baseline)!;
    // Nodes which take no space are not drawn: they add no rows.
    if (paired.width > 0) {
      minDepth = Math.min(minDepth, paired.depth);
      maxDepth = Math.max(maxDepth, paired.depth);
    }
    maxAbsDelta = Math.max(
      maxAbsDelta,
      Math.abs(paired.currentCumulative - paired.baselineCumulative),
    );
    return {
      id: paired.id,
      parentId: paired.parent?.id ?? -1,
      depth: paired.depth,
      name: source.name,
      selfValue: paired.currentSelf,
      cumulativeValue: paired.currentCumulative,
      parentCumulativeValue: paired.parent?.currentCumulative,
      // The pairing properties are equal on both sides; the others
      // (aggregatable and profile-specific) are taken from the current tree
      // where the node exists there, so that node actions reading them act
      // on the current tree. The flamegraph shows only the pairing ones, and
      // offers no host actions on the nodes only the baseline has.
      properties: source.properties,
      marker: source.marker,
      xStart: paired.x,
      xEnd: paired.x + paired.width,
      baselineSelfValue: paired.baselineSelf,
      baselineCumulativeValue: paired.baselineCumulative,
    };
  });

  return {
    nodes,
    unfilteredCumulativeValue: current.unfilteredCumulativeValue,
    allRootsCumulativeValue: current.allRootsCumulativeValue,
    minDepth,
    maxDepth,
    nodeActions: current.nodeActions,
    rootActions: current.rootActions,
    diff: {
      baselineUnfilteredCumulativeValue: baseline.unfilteredCumulativeValue,
      baselineAllRootsCumulativeValue: baseline.allRootsCumulativeValue,
      layoutWidth,
      maxAbsDelta,
    },
  };
}

// Whether nodes of the two trees of a diff are only paired up if they have
// the same value of `property`: the grouping (unaggregatable) properties,
// which the flamegraph operator also merges siblings by, minus the
// profile-specific ones. Only these are the same on both sides of a diff
// node; the others have a value per tree.
export function isPairingProperty(
  property: TreeExplorerPropertyDefinition,
): boolean {
  return !property.isAggregatable && property.profileSpecific !== true;
}

// Identifies a node among its siblings across trees: its name and pairing
// properties, sorted so that property order does not matter.
function pairingKey(node: TreeExplorerNode): string {
  const parts: string[] = [];
  for (const [key, property] of node.properties) {
    if (isPairingProperty(property)) {
      parts.push(`${key}\u0001${property.value}`);
    }
  }
  parts.sort();
  return [node.name, ...parts].join('\u0000');
}

function diffWidth(
  baseline: number,
  current: number,
  width: TreeExplorerDiffWidth,
): number {
  switch (width) {
    case 'COMBINED':
      return baseline + current;
    case 'CURRENT':
      return current;
    case 'BASELINE':
      return baseline;
    default:
      assertUnreachable(width);
  }
}

// How a value changed from `baseline` to `current`, as a score in [-1, 1]:
// positive if it grew, negative if it shrank, 0 if unchanged.
//  - ABSOLUTE: the change relative to `maxAbsDelta`, the largest change in
//    the tree, so the biggest movers stand out.
//  - RELATIVE: the change relative to the baseline value, saturating at a
//    doubling; new values score 1 and removed ones -1.
export function treeExplorerDiffScore(
  baseline: number,
  current: number,
  color: TreeExplorerDiffColor,
  maxAbsDelta: number,
): number {
  const delta = current - baseline;
  if (delta === 0) {
    return 0;
  }
  switch (color) {
    case 'ABSOLUTE':
      return maxAbsDelta === 0 ? 0 : clampScore(delta / maxAbsDelta);
    case 'RELATIVE':
      return baseline === 0 ? 1 : clampScore(delta / baseline);
    default:
      assertUnreachable(color);
  }
}

// The pprof web UI's diff palette (dotColor in pprof's
// internal/report/graph.go), tuned for the light fill of flamegraph nodes:
// green for scores < 0 (shrank), grey for 0, red for > 0 (grew).
const DIFF_SATURATION = 0.5; // Saturation at |score| = 1.
const DIFF_VALUE = 0.78; // HSV value of all colors.
const DIFF_SHIFT = 0.7; // Pushes scores away from grey to use more range.

export function treeExplorerDiffColor(score: number): Color {
  score = Number.isFinite(score) ? clampScore(score) : 0;
  // Desaturate near 0, so that small changes read as grey, not yellow.
  let saturation = DIFF_SATURATION;
  if (Math.abs(score) < 0.2) {
    saturation *= Math.abs(score) / 0.2;
  }
  const shifted = Math.sign(score) * Math.pow(Math.abs(score), 1 - DIFF_SHIFT);
  let r = DIFF_VALUE;
  let g = DIFF_VALUE;
  if (shifted < 0) {
    r *= 1 + saturation * shifted;
  } else {
    g *= 1 - saturation * shifted;
  }
  const b = DIFF_VALUE * (1 - saturation);
  const to255 = (x: number) => Math.round(x * 255);
  return new HSLColor(rgbToHsl([to255(r), to255(g), to255(b)]));
}

function clampScore(score: number): number {
  return Math.max(-1, Math.min(1, score));
}

// Formats a signed change in the metric's unit, e.g. "+1.50 MiB" or "-3".
export function displaySignedSize(delta: number, unit: string): string {
  if (delta === 0) {
    return displaySize(0, unit);
  }
  return `${delta > 0 ? '+' : '-'}${displaySize(Math.abs(delta), unit)}`;
}

// The change from `baseline` to `current` relative to `baseline`, in percent,
// or undefined when there is no baseline value to relate to.
export function changePercentage(
  baseline: number,
  current: number,
): number | undefined {
  if (baseline === 0) {
    return current === 0 ? 0 : undefined;
  }
  return ((current - baseline) / baseline) * 100;
}

// Formats the change from `baseline` to `current` relative to `baseline`,
// e.g. "+25.00%", or "new" when there is no baseline value to relate to.
export function displayChangePercentage(
  baseline: number,
  current: number,
): string {
  const change = changePercentage(baseline, current);
  if (change === undefined) {
    return 'new';
  }
  return `${change > 0 ? '+' : ''}${change.toFixed(2)}%`;
}

// Formats a value of a diff, e.g.
// "1.00 MiB → 1.50 MiB (+512.00 KiB, +50.00%)".
export function displayChange(
  baseline: number,
  current: number,
  unit: string,
): string {
  const from = displaySize(baseline, unit);
  const to = displaySize(current, unit);
  const delta = displaySignedSize(current - baseline, unit);
  const percentage = displayChangePercentage(baseline, current);
  return `${from} → ${to} (${delta}, ${percentage})`;
}
