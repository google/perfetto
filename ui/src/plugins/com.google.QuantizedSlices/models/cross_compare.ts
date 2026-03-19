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

// Cross Compare — Union-Find based pairwise comparison algorithm.
//
// Traces are nodes in a graph. Positive comparisons merge nodes into
// clusters (union-find). Negative comparisons mark inter-component
// separation. Transitivity: if A+B and B+C then A+C is implied.
// The pair selector prioritises the two largest unresolved components.

export function edgeKey(a: string, b: string): string {
  return a < b ? `${a}|${b}` : `${b}|${a}`;
}

// ── Union-Find ──

export class UnionFind {
  private parent = new Map<string, string>();
  private rankMap = new Map<string, number>();

  constructor(keys: string[]) {
    for (const k of keys) {
      this.parent.set(k, k);
      this.rankMap.set(k, 0);
    }
  }

  find(x: string): string {
    let root = x;
    while (this.parent.get(root) !== root) root = this.parent.get(root)!;
    let cur = x;
    while (cur !== root) {
      const next = this.parent.get(cur)!;
      this.parent.set(cur, root);
      cur = next;
    }
    return root;
  }

  union(a: string, b: string): void {
    const ra = this.find(a);
    const rb = this.find(b);
    if (ra === rb) return;
    const rankA = this.rankMap.get(ra)!;
    const rankB = this.rankMap.get(rb)!;
    if (rankA < rankB) {
      this.parent.set(ra, rb);
    } else if (rankA > rankB) {
      this.parent.set(rb, ra);
    } else {
      this.parent.set(rb, ra);
      this.rankMap.set(ra, rankA + 1);
    }
  }

  connected(a: string, b: string): boolean {
    return this.find(a) === this.find(b);
  }

  components(): Map<string, string[]> {
    const map = new Map<string, string[]>();
    for (const k of this.parent.keys()) {
      const root = this.find(k);
      let arr = map.get(root);
      if (!arr) {
        arr = [];
        map.set(root, arr);
      }
      arr.push(k);
    }
    return map;
  }
}

// ── Cross Compare State ──

export type ComparisonResult = 'positive' | 'negative';

export type HistoryEntry =
  | {type: 'compare'; keyA: string; keyB: string; result: ComparisonResult}
  | {type: 'discard'; key: string};

export interface CrossCompareState {
  uf: UnionFind;
  negativeEdges: Set<string>;
  comparisons: Map<string, ComparisonResult>;
  skippedPairs: Set<string>;
  discardedKeys: Set<string>;
  history: HistoryEntry[];
  traceKeys: string[];
  currentPair: [string, string] | null;
  selectedSide: 'left' | 'right' | null;
  isComplete: boolean;
}

export function createCrossCompareState(
  traceKeys: string[],
): CrossCompareState {
  const state: CrossCompareState = {
    uf: new UnionFind(traceKeys),
    negativeEdges: new Set(),
    comparisons: new Map(),
    skippedPairs: new Set(),
    discardedKeys: new Set(),
    history: [],
    traceKeys,
    currentPair: null,
    selectedSide: null,
    isComplete: false,
  };
  state.currentPair = nextPair(state);
  if (!state.currentPair) state.isComplete = true;
  return state;
}

// ── Core Operations ──

export function recordComparison(
  state: CrossCompareState,
  keyA: string,
  keyB: string,
  result: ComparisonResult,
): void {
  const ek = edgeKey(keyA, keyB);
  state.comparisons.set(ek, result);
  state.skippedPairs.delete(ek);
  state.history.push({type: 'compare', keyA, keyB, result});

  if (result === 'positive') {
    const rootA = state.uf.find(keyA);
    const rootB = state.uf.find(keyB);
    const toRekey: [string, string][] = [];
    for (const neg of state.negativeEdges) {
      const [x, y] = neg.split('|');
      if (x === rootA || x === rootB || y === rootA || y === rootB) {
        toRekey.push([x, y]);
      }
    }
    state.uf.union(keyA, keyB);
    for (const [x, y] of toRekey) {
      state.negativeEdges.delete(edgeKey(x, y));
      const rx = state.uf.find(x);
      const ry = state.uf.find(y);
      if (rx !== ry) state.negativeEdges.add(edgeKey(rx, ry));
    }
  } else if (result === 'negative') {
    const rootA = state.uf.find(keyA);
    const rootB = state.uf.find(keyB);
    if (rootA !== rootB) {
      state.negativeEdges.add(edgeKey(rootA, rootB));
    }
  }
}

export function skipCurrentPair(state: CrossCompareState): void {
  if (!state.currentPair) return;
  state.skippedPairs.add(edgeKey(state.currentPair[0], state.currentPair[1]));
}

export function discardTrace(state: CrossCompareState, key: string): void {
  state.discardedKeys.add(key);
  state.history.push({type: 'discard', key});
}

export function undoComparison(state: CrossCompareState): void {
  if (state.history.length === 0) return;
  const prev = state.history.slice(0, -1);
  state.uf = new UnionFind(state.traceKeys);
  state.negativeEdges.clear();
  state.comparisons.clear();
  state.skippedPairs.clear();
  state.discardedKeys.clear();
  state.history = [];
  for (const entry of prev) {
    if (entry.type === 'compare') {
      recordComparison(state, entry.keyA, entry.keyB, entry.result);
    } else {
      discardTrace(state, entry.key);
    }
  }
  state.currentPair = nextPair(state);
  state.isComplete = !state.currentPair;
  state.selectedSide = null;
}

export function nextPair(state: CrossCompareState): [string, string] | null {
  const comps = activeComponents(state);
  const sorted = [...comps.entries()].sort((a, b) => b[1].length - a[1].length);

  let fallback: [string, string] | null = null;
  for (let i = 0; i < sorted.length; i++) {
    for (let j = i + 1; j < sorted.length; j++) {
      const rootI = sorted[i][0];
      const rootJ = sorted[j][0];
      if (state.negativeEdges.has(edgeKey(rootI, rootJ))) continue;
      const pair = findUncomparedPair(
        state,
        sorted[i][1],
        sorted[j][1],
        state.skippedPairs,
      );
      if (pair) return pair;
      if (!fallback) {
        const skipped = findUncomparedPair(state, sorted[i][1], sorted[j][1]);
        if (skipped) fallback = skipped;
      }
    }
  }
  return fallback;
}

export function nextPairForAnchor(
  state: CrossCompareState,
  anchorKey: string,
): [string, string] | null {
  if (state.discardedKeys.has(anchorKey)) return null;
  const comps = activeComponents(state);
  const anchorRoot = state.uf.find(anchorKey);

  const sorted = [...comps.entries()]
    .filter(([root]) => root !== anchorRoot)
    .sort((a, b) => b[1].length - a[1].length);

  let fallback: [string, string] | null = null;
  for (const [root, members] of sorted) {
    if (state.negativeEdges.has(edgeKey(anchorRoot, root))) continue;
    for (const other of members) {
      const ek = edgeKey(anchorKey, other);
      if (!state.comparisons.has(ek) && !state.skippedPairs.has(ek)) {
        return [anchorKey, other];
      }
      if (!fallback && !state.comparisons.has(ek)) {
        fallback = [anchorKey, other];
      }
    }
  }
  return fallback;
}

function activeComponents(state: CrossCompareState): Map<string, string[]> {
  if (state.discardedKeys.size === 0) return state.uf.components();
  const comps = state.uf.components();
  const filtered = new Map<string, string[]>();
  for (const [root, members] of comps) {
    const active = members.filter((k) => !state.discardedKeys.has(k));
    if (active.length > 0) filtered.set(root, active);
  }
  return filtered;
}

function findUncomparedPair(
  state: CrossCompareState,
  membersA: string[],
  membersB: string[],
  exclude?: Set<string>,
): [string, string] | null {
  for (const a of membersA) {
    for (const b of membersB) {
      const ek = edgeKey(a, b);
      if (!state.comparisons.has(ek) && (!exclude || !exclude.has(ek))) {
        return [a, b];
      }
    }
  }
  return null;
}

// ── Progress ──

export function getProgress(state: CrossCompareState): {
  completed: number;
  total: number;
  pct: number;
} {
  const comps = activeComponents(state);
  const roots = [...comps.keys()];
  let total = 0;
  let resolved = 0;
  for (let i = 0; i < roots.length; i++) {
    for (let j = i + 1; j < roots.length; j++) {
      total++;
      if (state.negativeEdges.has(edgeKey(roots[i], roots[j]))) {
        resolved++;
      }
    }
  }
  const n = state.traceKeys.length - state.discardedKeys.size;
  const maxPairs = (n * (n - 1)) / 2;
  const mergedPairs = maxPairs - total;
  const totalWork = maxPairs;
  const completedWork = mergedPairs + resolved;
  return {
    completed: completedWork,
    total: totalWork,
    pct: totalWork > 0 ? Math.round((completedWork / totalWork) * 100) : 100,
  };
}

// ── Results ──

export interface CrossCompareResults {
  groups: string[][];
  discarded: string[];
}

export function getResults(state: CrossCompareState): CrossCompareResults {
  const comps = activeComponents(state);
  const groups = [...comps.values()].sort((a, b) => b.length - a.length);
  return {groups, discarded: [...state.discardedKeys]};
}
