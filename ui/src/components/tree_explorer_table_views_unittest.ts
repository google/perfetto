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

import {
  buildFlatExportString,
  computeFlatFunctions,
} from './tree_explorer_table_views';
import type {
  TreeExplorerData,
  TreeExplorerNode,
} from '../widgets/tree_explorer';

interface TestNode {
  readonly id: number;
  readonly parentId: number;
  readonly depth: number;
  readonly name: string;
  readonly selfValue: number;
  readonly cumulativeValue: number;
  readonly baselineSelfValue?: number;
  readonly baselineCumulativeValue?: number;
}

function makeData(nodes: ReadonlyArray<TestNode>): TreeExplorerData {
  const fullNodes: TreeExplorerNode[] = nodes.map((n) => ({
    ...n,
    properties: new Map(),
    xStart: 0,
    xEnd: 0,
  }));
  return {
    nodes: fullNodes,
    unfilteredCumulativeValue: 0,
    allRootsCumulativeValue: 0,
    minDepth: Math.min(0, ...nodes.map((n) => n.depth)),
    maxDepth: Math.max(0, ...nodes.map((n) => n.depth)),
    nodeActions: [],
    rootActions: [],
  };
}

// A diff whose nodes carry both trees' values.
function makeDiffData(nodes: ReadonlyArray<TestNode>): TreeExplorerData {
  const roots = nodes.filter((n) => n.parentId === -1);
  const sum = (values: number[]) => values.reduce((a, b) => a + b, 0);
  return {
    ...makeData(nodes),
    allRootsCumulativeValue: sum(roots.map((n) => n.cumulativeValue)),
    diff: {
      baselineUnfilteredCumulativeValue: 0,
      baselineAllRootsCumulativeValue: sum(
        roots.map((n) => n.baselineCumulativeValue ?? 0),
      ),
      layoutWidth: 0,
      maxAbsDelta: 0,
    },
  };
}

function byName(functions: ReturnType<typeof computeFlatFunctions>) {
  return new Map(functions.map((f) => [f.name, f]));
}

test('computeFlatFunctions sums self and total per name', () => {
  const fns = byName(
    computeFlatFunctions(
      makeData([
        // A -> {B, C}
        {
          id: 1,
          parentId: -1,
          depth: 1,
          name: 'A',
          selfValue: 1,
          cumulativeValue: 10,
        },
        {
          id: 2,
          parentId: 1,
          depth: 2,
          name: 'B',
          selfValue: 4,
          cumulativeValue: 4,
        },
        {
          id: 3,
          parentId: 1,
          depth: 2,
          name: 'C',
          selfValue: 5,
          cumulativeValue: 5,
        },
      ]),
    ),
  );
  expect(fns.get('A')).toEqual({name: 'A', self: 1, total: 10});
  expect(fns.get('B')).toEqual({name: 'B', self: 4, total: 4});
  expect(fns.get('C')).toEqual({name: 'C', self: 5, total: 5});
});

test('computeFlatFunctions counts recursive frames once per path', () => {
  // A -> B -> A: the inner A's cumulative value is already part of the outer
  // A's, so A's total must be 10, not 17.
  const fns = byName(
    computeFlatFunctions(
      makeData([
        {
          id: 1,
          parentId: -1,
          depth: 1,
          name: 'A',
          selfValue: 1,
          cumulativeValue: 10,
        },
        {
          id: 2,
          parentId: 1,
          depth: 2,
          name: 'B',
          selfValue: 2,
          cumulativeValue: 9,
        },
        {
          id: 3,
          parentId: 2,
          depth: 3,
          name: 'A',
          selfValue: 7,
          cumulativeValue: 7,
        },
      ]),
    ),
  );
  expect(fns.get('A')).toEqual({name: 'A', self: 8, total: 10});
  expect(fns.get('B')).toEqual({name: 'B', self: 2, total: 9});
});

test('computeFlatFunctions adds distinct paths through the same name', () => {
  // A appears under two different roots: both cumulative values count.
  const fns = byName(
    computeFlatFunctions(
      makeData([
        {
          id: 1,
          parentId: -1,
          depth: 1,
          name: 'R1',
          selfValue: 0,
          cumulativeValue: 4,
        },
        {
          id: 2,
          parentId: 1,
          depth: 2,
          name: 'A',
          selfValue: 4,
          cumulativeValue: 4,
        },
        {
          id: 3,
          parentId: -1,
          depth: 1,
          name: 'R2',
          selfValue: 0,
          cumulativeValue: 6,
        },
        {
          id: 4,
          parentId: 3,
          depth: 2,
          name: 'A',
          selfValue: 6,
          cumulativeValue: 6,
        },
      ]),
    ),
  );
  expect(fns.get('A')).toEqual({name: 'A', self: 10, total: 10});
});

test('computeFlatFunctions ignores the caller direction of pivot views', () => {
  // Negative depths are callers of the pivot: their values re-attribute the
  // same samples, so they are excluded from the aggregation.
  const fns = byName(
    computeFlatFunctions(
      makeData([
        {
          id: 1,
          parentId: -1,
          depth: -1,
          name: 'Caller',
          selfValue: 0,
          cumulativeValue: 10,
        },
        {
          id: 2,
          parentId: -1,
          depth: 1,
          name: 'Pivot',
          selfValue: 3,
          cumulativeValue: 10,
        },
        {
          id: 3,
          parentId: 2,
          depth: 2,
          name: 'Callee',
          selfValue: 7,
          cumulativeValue: 7,
        },
      ]),
    ),
  );
  expect(fns.get('Caller')).toBeUndefined();
  expect(fns.get('Pivot')).toEqual({name: 'Pivot', self: 3, total: 10});
  expect(fns.get('Callee')).toEqual({name: 'Callee', self: 7, total: 7});
});

// current: A -> B -> A; baseline: A -> {B -> A, C}. C was removed and the
// recursive A grew.
const DIFF_NODES: ReadonlyArray<TestNode> = [
  {
    id: 1,
    parentId: -1,
    depth: 1,
    name: 'A',
    selfValue: 1,
    cumulativeValue: 10,
    baselineSelfValue: 1,
    baselineCumulativeValue: 8,
  },
  {
    id: 2,
    parentId: 1,
    depth: 2,
    name: 'B',
    selfValue: 2,
    cumulativeValue: 9,
    baselineSelfValue: 2,
    baselineCumulativeValue: 5,
  },
  {
    id: 3,
    parentId: 2,
    depth: 3,
    name: 'A',
    selfValue: 7,
    cumulativeValue: 7,
    baselineSelfValue: 3,
    baselineCumulativeValue: 3,
  },
  {
    id: 4,
    parentId: 1,
    depth: 2,
    name: 'C',
    selfValue: 0,
    cumulativeValue: 0,
    baselineSelfValue: 2,
    baselineCumulativeValue: 2,
  },
];

test('computeFlatFunctions aggregates the baseline values of diffs', () => {
  const fns = byName(computeFlatFunctions(makeDiffData(DIFF_NODES)));
  // The recursive A counts once towards the totals of either tree.
  expect(fns.get('A')).toEqual({
    name: 'A',
    self: 8,
    total: 10,
    baselineSelf: 4,
    baselineTotal: 8,
  });
  expect(fns.get('B')).toEqual({
    name: 'B',
    self: 2,
    total: 9,
    baselineSelf: 2,
    baselineTotal: 5,
  });
  expect(fns.get('C')).toEqual({
    name: 'C',
    self: 0,
    total: 0,
    baselineSelf: 2,
    baselineTotal: 2,
  });
});

test('buildFlatExportString exports both trees of diffs', () => {
  // X is new: it has no relative change.
  const data = makeDiffData([
    ...DIFF_NODES,
    {
      id: 5,
      parentId: -1,
      depth: 1,
      name: 'X',
      selfValue: 5,
      cumulativeValue: 5,
    },
  ]);
  const lines = buildFlatExportString(
    data,
    {name: 'Size', unit: 'B'},
    'tsv',
  ).split('\n');
  expect(lines).toEqual([
    [
      'Name',
      'Baseline Self Size (B)',
      'Current Self Size (B)',
      'Change Self Size (B)',
      'Change Self %',
      'Baseline Total Size (B)',
      'Current Total Size (B)',
      'Change Total Size (B)',
      'Change Total %',
    ].join('\t'),
    // Biggest self growth first.
    ['X', '0', '5', '5', '', '0', '5', '5', ''].join('\t'),
    ['A', '4', '8', '4', '100.00', '8', '10', '2', '25.00'].join('\t'),
    ['B', '2', '2', '0', '0.00', '5', '9', '4', '80.00'].join('\t'),
    ['C', '2', '0', '-2', '-100.00', '2', '0', '-2', '-100.00'].join('\t'),
  ]);
});

test('buildFlatExportString exports single trees unchanged', () => {
  const lines = buildFlatExportString(
    makeData([
      {
        id: 1,
        parentId: -1,
        depth: 1,
        name: 'A',
        selfValue: 1,
        cumulativeValue: 3,
      },
      {
        id: 2,
        parentId: 1,
        depth: 2,
        name: 'B',
        selfValue: 2,
        cumulativeValue: 2,
      },
    ]),
    {name: 'Size', unit: 'B'},
    'tsv',
  ).split('\n');
  expect(lines[0]).toBe(
    ['Name', 'Self Size (B)', 'Self %', 'Total Size (B)', 'Total %'].join('\t'),
  );
  expect(lines.slice(1).map((l) => l.split('\t')[0])).toEqual(['B', 'A']);
});
