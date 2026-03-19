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

import {MergedSlice, Slice} from './types';

function tokenDistance(a: MergedSlice, b: MergedSlice): number {
  let d = 0;
  if (a.state !== b.state) {
    d += 4;
  } else if (a.state === 'Uninterruptible Sleep' && a.io_wait !== b.io_wait) {
    d += 2;
  }
  if (a.name !== b.name) {
    d += a.name === null || b.name === null ? 1 : 2;
  }
  if (a.blocked_function !== b.blocked_function) {
    d += a.blocked_function === null || b.blocked_function === null ? 0.5 : 1;
  }
  return d;
}

function mergeCost(a: MergedSlice, b: MergedSlice): number {
  const dist = tokenDistance(a, b);
  if (dist === 0) {
    return 0.01 * Math.log1p((a.dur + b.dur) / 1e6);
  }
  const loser = a.dur <= b.dur ? a : b;
  const loserWeight =
    Math.log1p(loser.dur / 1e6) *
    (1 +
      (loser.name !== null ? 1 : 0) +
      (loser.blocked_function !== null ? 1 : 0));
  return dist * loserWeight;
}

function mergeTwo(a: MergedSlice, b: MergedSlice): MergedSlice {
  const winner = a.dur >= b.dur ? a : b;
  return {
    ts: a.ts,
    tsRel: a.tsRel,
    dur: a.dur + b.dur,
    state: winner.state,
    io_wait: winner.io_wait,
    name: winner.name,
    depth: winner.depth,
    blocked_function: winner.blocked_function,
    _merged: (a._merged || 1) + (b._merged || 1),
  };
}

// Determine which lengths to cache — power-of-2 steps plus the original.
function isCheckpoint(len: number, origN: number): boolean {
  if (len === origN || len === 2) return true;
  // Cache at powers of 2 and every 50 steps for fine-grained slider control.
  return (len & (len - 1)) === 0 || len % 50 === 0;
}

// Linked-list node for in-place merging. Avoids O(n) array copies per step.
interface LLNode {
  data: MergedSlice;
  prev: number; // index into nodes[], -1 if head
  next: number; // index into nodes[], -1 if tail
  alive: boolean;
  cost: number; // merge cost with the next alive node
}

function snapshot(nodes: LLNode[], head: number): MergedSlice[] {
  const result: MergedSlice[] = [];
  let cur = head;
  while (cur !== -1) {
    result.push({...nodes[cur].data});
    cur = nodes[cur].next;
  }
  return result;
}

export interface MergeCache {
  cache: Map<number, MergedSlice[]>;
  sortedKeys: number[]; // sorted ascending for binary search
}

export function buildMergeCache(rawData: Slice[]): MergeCache {
  const cache = new Map<number, MergedSlice[]>();
  if (rawData.length === 0) return {cache, sortedKeys: []};

  const n = rawData.length;
  const nodes: LLNode[] = rawData.map((d, i) => ({
    data: {
      ...d,
      tsRel: d.ts - rawData[0].ts,
      _merged: 1,
    },
    prev: i - 1,
    next: i + 1 < n ? i + 1 : -1,
    alive: true,
    cost: 0,
  }));

  // Compute initial merge costs.
  for (let i = 0; i < n; i++) {
    if (nodes[i].next !== -1) {
      nodes[i].cost = mergeCost(nodes[i].data, nodes[nodes[i].next].data);
    } else {
      nodes[i].cost = Infinity;
    }
  }

  const head = 0;
  let aliveCount = n;

  // Cache original length.
  cache.set(n, snapshot(nodes, head));

  while (aliveCount > 2) {
    // Find min-cost pair by scanning alive nodes.
    let bestIdx = -1;
    let bestCost = Infinity;
    let cur = head;
    while (cur !== -1) {
      if (nodes[cur].next !== -1 && nodes[cur].cost < bestCost) {
        bestCost = nodes[cur].cost;
        bestIdx = cur;
      }
      cur = nodes[cur].next;
    }
    if (bestIdx === -1) break;

    // Merge bestIdx with its next neighbor.
    const nextIdx = nodes[bestIdx].next;
    nodes[bestIdx].data = mergeTwo(nodes[bestIdx].data, nodes[nextIdx].data);

    // Remove nextIdx from linked list.
    nodes[nextIdx].alive = false;
    nodes[bestIdx].next = nodes[nextIdx].next;
    if (nodes[nextIdx].next !== -1) {
      nodes[nodes[nextIdx].next].prev = bestIdx;
    }
    aliveCount--;

    // Recompute costs for bestIdx and its predecessor.
    if (nodes[bestIdx].next !== -1) {
      nodes[bestIdx].cost = mergeCost(
        nodes[bestIdx].data,
        nodes[nodes[bestIdx].next].data,
      );
    } else {
      nodes[bestIdx].cost = Infinity;
    }
    if (nodes[bestIdx].prev !== -1) {
      const prevIdx = nodes[bestIdx].prev;
      nodes[prevIdx].cost = mergeCost(nodes[prevIdx].data, nodes[bestIdx].data);
    }

    if (isCheckpoint(aliveCount, n)) {
      cache.set(aliveCount, snapshot(nodes, head));
    }
  }

  // Always cache the minimum (2 elements).
  if (!cache.has(2) && aliveCount === 2) {
    cache.set(aliveCount, snapshot(nodes, head));
  }

  const sortedKeys = [...cache.keys()].sort((a, b) => a - b);
  return {cache, sortedKeys};
}

// Binary search for smallest key >= target in sorted array.
function ceilSearch(arr: number[], target: number): number {
  let lo = 0;
  let hi = arr.length - 1;
  let result = arr[hi]; // fallback to largest
  while (lo <= hi) {
    const mid = (lo + hi) >> 1;
    if (arr[mid] >= target) {
      result = arr[mid];
      hi = mid - 1;
    } else {
      lo = mid + 1;
    }
  }
  return result;
}

export function getCompressed(
  mergeCache: MergeCache,
  _origN: number,
  target: number,
): MergedSlice[] {
  const t = Math.max(2, target);
  if (mergeCache.sortedKeys.length === 0) return [];
  const best = ceilSearch(mergeCache.sortedKeys, t);
  return mergeCache.cache.get(best) ?? mergeCache.cache.get(2) ?? [];
}
