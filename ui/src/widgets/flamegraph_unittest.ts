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

import {buildFlamegraphExportString} from './flamegraph';
import type {
  TreeExplorerData,
  TreeExplorerNode,
  TreeExplorerPropertyDefinition,
} from './tree_explorer';
import {buildTreeExplorerDiff} from './tree_explorer_diff';

const METRIC = {name: 'Size', unit: 'B'};

// A single root with one child per entry of `children` (name, self value).
function makeTree(
  rootSelf: number,
  children: ReadonlyArray<[string, number]>,
): TreeExplorerData {
  const total = children.reduce((sum, [_, self]) => sum + self, rootSelf);
  const node = (
    id: number,
    parentId: number,
    depth: number,
    name: string,
    self: number,
    cumulative: number,
  ): TreeExplorerNode => ({
    id,
    parentId,
    depth,
    name,
    selfValue: self,
    cumulativeValue: cumulative,
    properties: new Map(),
    xStart: 0,
    xEnd: 0,
  });
  return {
    nodes: [
      node(0, -1, 1, 'A', rootSelf, total),
      ...children.map(([name, self], i) => node(i + 1, 0, 2, name, self, self)),
    ],
    unfilteredCumulativeValue: total,
    allRootsCumulativeValue: total,
    minDepth: 0,
    maxDepth: children.length > 0 ? 2 : 1,
    nodeActions: [],
    rootActions: [],
  };
}

test('buildFlamegraphExportString exports single trees', () => {
  const lines = buildFlamegraphExportString(
    makeTree(1, [['B', 4]]),
    METRIC,
    'tsv',
  ).split('\n');
  expect(lines).toEqual([
    'Id\tParent Id\tDepth\tName\tCumulative Size (B)\tSelf Size (B)',
    '0\t-1\t1\tA\t5\t1',
    '1\t0\t2\tB\t4\t4',
  ]);
});

test('buildFlamegraphExportString exports both trees of diffs', () => {
  const current = makeTree(1, [
    ['B', 4],
    ['C', 5],
  ]);
  const baseline = makeTree(1, [
    ['B', 2],
    ['D', 3],
  ]);
  const lines = buildFlamegraphExportString(
    buildTreeExplorerDiff(current, baseline, 'COMBINED'),
    METRIC,
    'tsv',
  ).split('\n');
  expect(lines).toEqual([
    [
      'Id',
      'Parent Id',
      'Depth',
      'Name',
      'Baseline Cumulative Size (B)',
      'Current Cumulative Size (B)',
      'Change Cumulative Size (B)',
      'Baseline Self Size (B)',
      'Current Self Size (B)',
      'Change Self Size (B)',
    ].join('\t'),
    '0\t-1\t1\tA\t6\t10\t4\t1\t1\t0',
    '1\t0\t2\tB\t2\t4\t2\t2\t4\t2',
    '2\t0\t2\tC\t0\t5\t5\t0\t5\t5',
    // Only in the baseline.
    '3\t0\t2\tD\t3\t0\t-3\t3\t0\t-3',
  ]);
});

// A single node with a grouping, a profile-specific and an aggregatable
// property.
function makeNodeWithProperties(
  value: number,
  hash: string,
  count: number,
): TreeExplorerData {
  const property = (
    displayName: string,
    propertyValue: string,
    isAggregatable: boolean,
    profileSpecific?: boolean,
  ): TreeExplorerPropertyDefinition => ({
    displayName,
    value: propertyValue,
    isVisible: true,
    isAggregatable,
    profileSpecific,
  });
  return {
    nodes: [
      {
        id: 0,
        parentId: -1,
        depth: 1,
        name: 'A',
        selfValue: value,
        cumulativeValue: value,
        properties: new Map([
          ['heap', property('Heap', 'app', false)],
          ['hash', property('Hash', hash, false, true)],
          ['count', property('Count', `${count}`, true)],
        ]),
        xStart: 0,
        xEnd: value,
      },
    ],
    unfilteredCumulativeValue: value,
    allRootsCumulativeValue: value,
    minDepth: 0,
    maxDepth: 1,
    nodeActions: [],
    rootActions: [],
  };
}

test('buildFlamegraphExportString exports properties of single trees', () => {
  const lines = buildFlamegraphExportString(
    makeNodeWithProperties(30, 'c1', 3),
    METRIC,
    'tsv',
  ).split('\n');
  expect(lines).toEqual([
    'Id\tParent Id\tDepth\tName\tHeap\tHash\tCumulative Size (B)\t' +
      'Self Size (B)\tCount',
    '0\t-1\t1\tA\tapp\tc1\t30\t30\t3',
  ]);
});

test('buildFlamegraphExportString exports only properties of both trees of diffs', () => {
  const diff = buildTreeExplorerDiff(
    makeNodeWithProperties(30, 'c1', 3),
    makeNodeWithProperties(10, 'b1', 1),
    'COMBINED',
  );
  const lines = buildFlamegraphExportString(diff, METRIC, 'tsv').split('\n');
  // The hash and count differ between the trees, so neither is exported.
  expect(lines).toEqual([
    [
      'Id',
      'Parent Id',
      'Depth',
      'Name',
      'Heap',
      'Baseline Cumulative Size (B)',
      'Current Cumulative Size (B)',
      'Change Cumulative Size (B)',
      'Baseline Self Size (B)',
      'Current Self Size (B)',
      'Change Self Size (B)',
    ].join('\t'),
    '0\t-1\t1\tA\tapp\t10\t30\t20\t10\t30\t20',
  ]);
});
