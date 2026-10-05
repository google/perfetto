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

import type {TreeExplorerData, TreeExplorerNode} from './tree_explorer';
import {
  buildTreeExplorerDiff,
  changePercentage,
  displayChange,
  displayChangePercentage,
  displaySignedSize,
  isPairingProperty,
  treeExplorerDiffColor,
  treeExplorerDiffScore,
} from './tree_explorer_diff';

interface Spec {
  readonly name: string;
  readonly self?: number;
  readonly props?: Readonly<Record<string, string>>;
  readonly children?: ReadonlyArray<Spec>;
}

// The kinds of properties other than regular grouping ones, by key.
type PropertyKinds = Readonly<
  Record<string, 'aggregatable' | 'profileSpecific'>
>;

// Builds tree explorer data the way the operator returns it: breadth-first
// (parents before children), roots at depth 1 (`roots`) and -1 (`upward`).
function makeTree(
  roots: ReadonlyArray<Spec>,
  upward: ReadonlyArray<Spec> = [],
  kinds: PropertyKinds = {},
): TreeExplorerData {
  const cumulative = (s: Spec): number =>
    (s.self ?? 0) +
    (s.children ?? []).reduce((sum, c) => sum + cumulative(c), 0);
  const queue = [
    ...roots.map((spec) => ({spec, parentId: -1, depth: 1})),
    ...upward.map((spec) => ({spec, parentId: -1, depth: -1})),
  ];
  const nodes: TreeExplorerNode[] = [];
  for (let id = 0; id < queue.length; id++) {
    const {spec, parentId, depth} = queue[id];
    nodes.push({
      id,
      parentId,
      depth,
      name: spec.name,
      selfValue: spec.self ?? 0,
      cumulativeValue: cumulative(spec),
      properties: new Map(
        Object.entries(spec.props ?? {}).map(([key, value]) => [
          key,
          {
            displayName: key,
            value,
            isVisible: true,
            isAggregatable: kinds[key] === 'aggregatable',
            profileSpecific: kinds[key] === 'profileSpecific',
          },
        ]),
      ),
      xStart: 0,
      xEnd: 0,
    });
    for (const child of spec.children ?? []) {
      queue.push({spec: child, parentId: id, depth: depth + Math.sign(depth)});
    }
  }
  // Like the fetcher: bottom-up trees only have upward nodes.
  const total = (roots.length > 0 ? roots : upward).reduce(
    (sum, s) => sum + cumulative(s),
    0,
  );
  return {
    nodes,
    unfilteredCumulativeValue: total,
    allRootsCumulativeValue: total,
    minDepth: Math.min(0, ...nodes.map((n) => n.depth)),
    maxDepth: Math.max(0, ...nodes.map((n) => n.depth)),
    nodeActions: [],
    rootActions: [],
  };
}

// Nodes by their path of names from the root, e.g. 'A/B'; paths of upward
// nodes start with '^'.
function byPath(data: TreeExplorerData): Map<string, TreeExplorerNode> {
  const byId = new Map(data.nodes.map((n) => [n.id, n]));
  const path = (n: TreeExplorerNode): string => {
    const parent = byId.get(n.parentId);
    if (parent !== undefined) {
      return `${path(parent)}/${n.name}`;
    }
    return n.depth < 0 ? `^${n.name}` : n.name;
  };
  const result = new Map<string, TreeExplorerNode>();
  for (const n of data.nodes) {
    const p = path(n);
    expect(result.has(p)).toBe(false);
    result.set(p, n);
  }
  return result;
}

function values(n: TreeExplorerNode | undefined) {
  return {
    self: n?.selfValue,
    cumulative: n?.cumulativeValue,
    baselineSelf: n?.baselineSelfValue,
    baselineCumulative: n?.baselineCumulativeValue,
  };
}

function extent(n: TreeExplorerNode | undefined) {
  return [n?.xStart, n?.xEnd];
}

// current: A -> {B, C}; baseline: A -> {B, D}.
const CURRENT = makeTree([
  {
    name: 'A',
    self: 1,
    children: [
      {name: 'B', self: 4},
      {name: 'C', self: 5},
    ],
  },
]);
const BASELINE = makeTree([
  {
    name: 'A',
    self: 1,
    children: [
      {name: 'B', self: 2},
      {name: 'D', self: 3},
    ],
  },
]);

describe('buildTreeExplorerDiff', () => {
  test('pairs nodes by path and keeps added and removed ones', () => {
    const nodes = byPath(buildTreeExplorerDiff(CURRENT, BASELINE, 'COMBINED'));
    expect([...nodes.keys()].sort()).toEqual(['A', 'A/B', 'A/C', 'A/D']);
    expect(values(nodes.get('A'))).toEqual({
      self: 1,
      cumulative: 10,
      baselineSelf: 1,
      baselineCumulative: 6,
    });
    expect(values(nodes.get('A/B'))).toEqual({
      self: 4,
      cumulative: 4,
      baselineSelf: 2,
      baselineCumulative: 2,
    });
    // Only in the current tree.
    expect(values(nodes.get('A/C'))).toEqual({
      self: 5,
      cumulative: 5,
      baselineSelf: 0,
      baselineCumulative: 0,
    });
    // Only in the baseline tree.
    expect(values(nodes.get('A/D'))).toEqual({
      self: 0,
      cumulative: 0,
      baselineSelf: 3,
      baselineCumulative: 3,
    });
  });

  test('summarizes both trees', () => {
    const diff = buildTreeExplorerDiff(CURRENT, BASELINE, 'COMBINED');
    expect(diff.allRootsCumulativeValue).toBe(10);
    expect(diff.unfilteredCumulativeValue).toBe(10);
    expect(diff.diff).toEqual({
      baselineAllRootsCumulativeValue: 6,
      baselineUnfilteredCumulativeValue: 6,
      layoutWidth: 16,
      // A/C grew by 5: more than the root (+4) or any other node.
      maxAbsDelta: 5,
    });
    expect(diff.minDepth).toBe(0);
    expect(diff.maxDepth).toBe(2);
  });

  test('lists parents first with ids matching positions', () => {
    const {nodes} = buildTreeExplorerDiff(CURRENT, BASELINE, 'COMBINED');
    nodes.forEach((n, i) => {
      expect(n.id).toBe(i);
      expect(n.parentId).toBeLessThan(i);
    });
  });

  test('COMBINED widths show both trees, widest siblings first', () => {
    const nodes = byPath(buildTreeExplorerDiff(CURRENT, BASELINE, 'COMBINED'));
    expect(extent(nodes.get('A'))).toEqual([0, 16]);
    expect(extent(nodes.get('A/B'))).toEqual([0, 6]);
    expect(extent(nodes.get('A/C'))).toEqual([6, 11]);
    expect(extent(nodes.get('A/D'))).toEqual([11, 14]);
  });

  test('CURRENT widths hide baseline-only nodes', () => {
    const diff = buildTreeExplorerDiff(CURRENT, BASELINE, 'CURRENT');
    const nodes = byPath(diff);
    expect(diff.diff?.layoutWidth).toBe(10);
    expect(extent(nodes.get('A'))).toEqual([0, 10]);
    expect(extent(nodes.get('A/C'))).toEqual([0, 5]);
    expect(extent(nodes.get('A/B'))).toEqual([5, 9]);
    expect(extent(nodes.get('A/D'))).toEqual([9, 9]);
  });

  test('BASELINE widths hide current-only nodes', () => {
    const diff = buildTreeExplorerDiff(CURRENT, BASELINE, 'BASELINE');
    const nodes = byPath(diff);
    expect(diff.diff?.layoutWidth).toBe(6);
    expect(extent(nodes.get('A/D'))).toEqual([0, 3]);
    expect(extent(nodes.get('A/B'))).toEqual([3, 5]);
    expect(extent(nodes.get('A/C'))).toEqual([5, 5]);
  });

  test('hidden nodes do not extend the depth range', () => {
    const current = makeTree([{name: 'A', self: 1}]);
    const baseline = makeTree([
      {name: 'A', children: [{name: 'B', children: [{name: 'C', self: 1}]}]},
    ]);
    expect(buildTreeExplorerDiff(current, baseline, 'COMBINED').maxDepth).toBe(
      3,
    );
    expect(buildTreeExplorerDiff(current, baseline, 'CURRENT').maxDepth).toBe(
      1,
    );
  });

  test('children nest within their parents', () => {
    const current = makeTree([
      {
        name: 'main',
        children: [
          {name: 'a', self: 3, children: [{name: 'x', self: 7}]},
          {name: 'b', self: 1},
        ],
      },
      {name: 'other', self: 2},
    ]);
    const baseline = makeTree([
      {
        name: 'main',
        children: [
          {name: 'a', children: [{name: 'y', self: 4}]},
          {name: 'c', self: 9},
        ],
      },
    ]);
    for (const width of ['COMBINED', 'CURRENT', 'BASELINE'] as const) {
      const {nodes} = buildTreeExplorerDiff(current, baseline, width);
      for (const n of nodes) {
        const parent = nodes[n.parentId];
        if (parent !== undefined) {
          expect(n.xStart).toBeGreaterThanOrEqual(parent.xStart);
          expect(n.xEnd).toBeLessThanOrEqual(parent.xEnd);
        }
      }
    }
  });

  test('does not pair same-named nodes under different parents', () => {
    const current = makeTree([
      {name: 'A', children: [{name: 'X', self: 1}]},
      {name: 'B', children: [{name: 'X', self: 2}]},
    ]);
    const baseline = makeTree([{name: 'A', children: [{name: 'X', self: 5}]}]);
    const nodes = byPath(buildTreeExplorerDiff(current, baseline, 'COMBINED'));
    expect(values(nodes.get('A/X'))).toMatchObject({
      cumulative: 1,
      baselineCumulative: 5,
    });
    expect(values(nodes.get('B/X'))).toMatchObject({
      cumulative: 2,
      baselineCumulative: 0,
    });
  });

  test('pairs by grouping properties, except profile-specific ones', () => {
    const kinds: PropertyKinds = {hash: 'profileSpecific'};
    const current = makeTree(
      [
        {name: 'Foo', self: 3, props: {heap: 'app', hash: 'c1'}},
        {name: 'Foo', self: 1, props: {heap: 'zygote', hash: 'c2'}},
      ],
      [],
      kinds,
    );
    const baseline = makeTree(
      [
        {name: 'Foo', self: 2, props: {heap: 'app', hash: 'b1'}},
        {name: 'Bar', self: 4, props: {heap: 'app', hash: 'b2'}},
      ],
      [],
      kinds,
    );
    const {nodes} = buildTreeExplorerDiff(current, baseline, 'COMBINED');
    const find = (name: string, heap: string) =>
      nodes.find(
        (n) => n.name === name && n.properties.get('heap')?.value === heap,
      );
    const fooApp = find('Foo', 'app');
    expect(values(fooApp)).toMatchObject({
      cumulative: 3,
      baselineCumulative: 2,
    });
    // Profile-specific values come from the current tree when possible...
    expect(fooApp?.properties.get('hash')?.value).toBe('c1');
    expect(values(find('Foo', 'zygote'))).toMatchObject({
      cumulative: 1,
      baselineCumulative: 0,
    });
    // ...and from the baseline for nodes only it has.
    const bar = find('Bar', 'app');
    expect(values(bar)).toMatchObject({cumulative: 0, baselineCumulative: 4});
    expect(bar?.properties.get('hash')?.value).toBe('b2');
    expect(nodes.length).toBe(3);
  });

  test('does not pair nodes differing in a regular grouping property', () => {
    const current = makeTree([{name: 'Foo', self: 3, props: {hash: 'c1'}}]);
    const baseline = makeTree([{name: 'Foo', self: 2, props: {hash: 'b1'}}]);
    const {nodes} = buildTreeExplorerDiff(current, baseline, 'COMBINED');
    expect(nodes.length).toBe(2);
  });

  test('pairs nodes differing in aggregatable properties', () => {
    const kinds: PropertyKinds = {count: 'aggregatable'};
    const current = makeTree(
      [{name: 'Foo', self: 3, props: {count: '3'}}],
      [],
      kinds,
    );
    const baseline = makeTree(
      [{name: 'Foo', self: 2, props: {count: '2'}}],
      [],
      kinds,
    );
    const {nodes} = buildTreeExplorerDiff(current, baseline, 'COMBINED');
    expect(nodes.length).toBe(1);
    expect(values(nodes[0])).toMatchObject({
      cumulative: 3,
      baselineCumulative: 2,
    });
  });

  test('sums siblings of a tree which pair up alike', () => {
    // Differ only in a profile-specific property, so they pair up alike.
    const kinds: PropertyKinds = {hash: 'profileSpecific'};
    const current = makeTree(
      [
        {name: 'Foo', self: 3, props: {hash: 'c1'}},
        {name: 'Foo', self: 1, props: {hash: 'c2'}},
      ],
      [],
      kinds,
    );
    const baseline = makeTree(
      [{name: 'Foo', self: 2, props: {hash: 'b1'}}],
      [],
      kinds,
    );
    const {nodes} = buildTreeExplorerDiff(current, baseline, 'COMBINED');
    expect(nodes.length).toBe(1);
    expect(values(nodes[0])).toEqual({
      self: 4,
      cumulative: 4,
      baselineSelf: 2,
      baselineCumulative: 2,
    });
    expect(extent(nodes[0])).toEqual([0, 6]);
  });

  test('keeps the two directions of a pivot apart', () => {
    const current = makeTree(
      [{name: 'P', self: 3}],
      [{name: 'P', children: [{name: 'Q', self: 3}]}],
    );
    const baseline = makeTree(
      [{name: 'P', self: 1}],
      [{name: 'P', children: [{name: 'Q', self: 1}]}],
    );
    const diff = buildTreeExplorerDiff(current, baseline, 'COMBINED');
    const nodes = byPath(diff);
    expect(nodes.get('P')?.depth).toBe(1);
    expect(nodes.get('^P')?.depth).toBe(-1);
    expect(nodes.get('^P/Q')?.depth).toBe(-2);
    expect(values(nodes.get('^P/Q'))).toMatchObject({
      cumulative: 3,
      baselineCumulative: 1,
    });
    // Both strips start at x = 0.
    expect(extent(nodes.get('P'))).toEqual([0, 4]);
    expect(extent(nodes.get('^P'))).toEqual([0, 4]);
    expect(diff.minDepth).toBe(-2);
    expect(diff.maxDepth).toBe(1);
  });

  test('diffs bottom-up trees', () => {
    const current = makeTree(
      [],
      [{name: 'leaf', children: [{name: 'a', self: 3}]}],
    );
    const baseline = makeTree(
      [],
      [
        {
          name: 'leaf',
          children: [
            {name: 'a', self: 1},
            {name: 'b', self: 2},
          ],
        },
      ],
    );
    const diff = buildTreeExplorerDiff(current, baseline, 'COMBINED');
    const nodes = byPath(diff);
    expect([...nodes.keys()].sort()).toEqual(['^leaf', '^leaf/a', '^leaf/b']);
    expect(values(nodes.get('^leaf/b'))).toMatchObject({
      cumulative: 0,
      baselineCumulative: 2,
    });
    expect(extent(nodes.get('^leaf'))).toEqual([0, 6]);
    expect(extent(nodes.get('^leaf/a'))).toEqual([0, 4]);
    expect(extent(nodes.get('^leaf/b'))).toEqual([4, 6]);
    expect(diff.allRootsCumulativeValue).toBe(3);
    expect(diff.diff).toMatchObject({
      baselineAllRootsCumulativeValue: 3,
      layoutWidth: 6,
    });
    expect(diff.minDepth).toBe(-2);
    expect(diff.maxDepth).toBe(0);
  });

  test('handles empty trees', () => {
    const empty = makeTree([]);
    const diff = buildTreeExplorerDiff(CURRENT, empty, 'COMBINED');
    expect(diff.nodes.length).toBe(3);
    for (const n of diff.nodes) {
      expect(n.baselineCumulativeValue).toBe(0);
    }
    expect(buildTreeExplorerDiff(empty, empty, 'COMBINED').nodes).toEqual([]);
  });
});

test('isPairingProperty', () => {
  const property = (isAggregatable: boolean, profileSpecific?: boolean) => ({
    displayName: 'p',
    value: 'v',
    isVisible: true,
    isAggregatable,
    profileSpecific,
  });
  expect(isPairingProperty(property(false))).toBe(true);
  expect(isPairingProperty(property(false, false))).toBe(true);
  expect(isPairingProperty(property(false, true))).toBe(false);
  expect(isPairingProperty(property(true))).toBe(false);
});

describe('treeExplorerDiffScore', () => {
  test('ABSOLUTE scores the change against the largest change', () => {
    expect(treeExplorerDiffScore(6, 10, 'ABSOLUTE', 8)).toBe(0.5);
    expect(treeExplorerDiffScore(10, 6, 'ABSOLUTE', 8)).toBe(-0.5);
    expect(treeExplorerDiffScore(5, 5, 'ABSOLUTE', 8)).toBe(0);
    expect(treeExplorerDiffScore(0, 20, 'ABSOLUTE', 8)).toBe(1);
    expect(treeExplorerDiffScore(5, 5, 'ABSOLUTE', 0)).toBe(0);
  });

  test('RELATIVE scores the change against the baseline value', () => {
    expect(treeExplorerDiffScore(10, 15, 'RELATIVE', 100)).toBe(0.5);
    expect(treeExplorerDiffScore(10, 5, 'RELATIVE', 100)).toBe(-0.5);
    expect(treeExplorerDiffScore(10, 40, 'RELATIVE', 100)).toBe(1);
    expect(treeExplorerDiffScore(0, 5, 'RELATIVE', 100)).toBe(1);
    expect(treeExplorerDiffScore(5, 0, 'RELATIVE', 100)).toBe(-1);
    expect(treeExplorerDiffScore(0, 0, 'RELATIVE', 100)).toBe(0);
  });
});

describe('treeExplorerDiffColor', () => {
  // cssString is "rgb(R G B)".
  const rgb = (score: number): [number, number, number] => {
    const css = treeExplorerDiffColor(score).cssString;
    const match = css.match(/rgb\((\d+) (\d+) (\d+)\)/);
    if (match === null) throw new Error(`Unexpected color: ${css}`);
    return [Number(match[1]), Number(match[2]), Number(match[3])];
  };

  test('is grey when unchanged', () => {
    const [r, g, b] = rgb(0);
    expect(r).toBe(g);
    expect(g).toBe(b);
  });

  test('is red when grown and green when shrunk', () => {
    const [r1, g1, b1] = rgb(1);
    expect(r1).toBeGreaterThan(g1);
    expect(r1).toBeGreaterThan(b1);
    const [r2, g2, b2] = rgb(-1);
    expect(g2).toBeGreaterThan(r2);
    expect(g2).toBeGreaterThan(b2);
  });

  test('intensifies with the size of the change', () => {
    expect(rgb(0.9)[1]).toBeLessThan(rgb(0.3)[1]);
    expect(rgb(-0.9)[0]).toBeLessThan(rgb(-0.3)[0]);
  });

  test('clamps out of range and non-finite scores', () => {
    expect(rgb(5)).toEqual(rgb(1));
    expect(rgb(-5)).toEqual(rgb(-1));
    expect(rgb(NaN)).toEqual(rgb(0));
  });
});

describe('diff formatters', () => {
  test('displaySignedSize formats zero, positive, and negative deltas', () => {
    expect(displaySignedSize(0, 'B')).toBe('0 B');
    expect(displaySignedSize(1024, 'B')).toBe('+1 KiB');
    expect(displaySignedSize(-2048, 'B')).toBe('-2 KiB');
    expect(displaySignedSize(1536, 'B')).toBe('+1.50 KiB');
  });

  test('changePercentage handles zero baseline and normal values', () => {
    expect(changePercentage(0, 0)).toBe(0);
    expect(changePercentage(0, 100)).toBeUndefined();
    expect(changePercentage(100, 150)).toBe(50);
    expect(changePercentage(100, 50)).toBe(-50);
  });

  test('displayChangePercentage formats relative change and new values', () => {
    expect(displayChangePercentage(0, 100)).toBe('new');
    expect(displayChangePercentage(100, 125)).toBe('+25.00%');
    expect(displayChangePercentage(100, 75)).toBe('-25.00%');
  });

  test('displayChange formats full diff change string', () => {
    expect(displayChange(100, 150, 'count')).toBe('100 → 150 (+50, +50.00%)');
  });
});
