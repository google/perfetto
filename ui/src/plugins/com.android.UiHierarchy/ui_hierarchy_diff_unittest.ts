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

import type {UiHierarchyNode} from './ui_hierarchy_data';
import {
  changedWithAncestors,
  diffBaseIndex,
  diffSections,
  diffTrees,
  hasDiff,
} from './ui_hierarchy_diff';
import {type PropSection, PropRowsBuilder} from './ui_hierarchy_props';

function node(
  nodeId: string,
  parentNodeId?: string,
  text?: string,
): UiHierarchyNode {
  return {
    rowId: 0,
    ts: 0n,
    dur: 0n,
    windowId: 1,
    nodeId,
    parentNodeId,
    childIndex: 0,
    kind: 1,
    kindName: 'View',
    name: `n${nodeId}`,
    boundsLeft: 0,
    boundsTop: 0,
    boundsRight: 10,
    boundsBottom: 10,
    alpha: 1,
    flags: 0n,
    isVisible: true,
    isClickable: false,
    isFocused: false,
    isTextRedacted: false,
    text,
  };
}

const sectionsOf = (n: UiHierarchyNode): PropSection[] => [
  {
    title: 'View',
    rows: new PropRowsBuilder()
      .text('Name', n.name)
      .text('Text', n.text)
      .build(),
  },
];

describe('diffBaseIndex', () => {
  test('a pinned base before the current snapshot', () => {
    expect(diffBaseIndex(9, 4)).toBe(4);
  });
  test('else the previous snapshot', () => {
    expect(diffBaseIndex(9, undefined)).toBe(8);
    expect(diffBaseIndex(4, 4)).toBe(3);
    expect(diffBaseIndex(3, 4)).toBe(2);
    expect(diffBaseIndex(0, undefined)).toBeUndefined();
    expect(diffBaseIndex(0, 0)).toBeUndefined();
  });
});

describe('diffSections', () => {
  test('marks changed, added and removed rows in place', () => {
    const prev: PropSection[] = [
      {
        title: 'Window',
        rows: new PropRowsBuilder()
          .text('Frame', 'a')
          .text('Alpha', '0.5')
          .text('Visible', 'true')
          .build(),
      },
    ];
    const cur: PropSection[] = [
      {
        title: 'Window',
        rows: new PropRowsBuilder()
          .text('Frame', 'b')
          .text('Visible', 'true')
          .text('Layer id', '80')
          .build(),
      },
    ];
    const [window] = diffSections(prev, cur);
    expect(window.rows.map((r) => [r.label, r.diff?.kind])).toEqual([
      ['Frame', 'changed'],
      ['Alpha', 'removed'],
      ['Visible', undefined],
      ['Layer id', 'added'],
    ]);
    expect(window.rows[0].diff).toEqual({
      kind: 'changed',
      previous: {kind: 'text', text: 'a'},
    });
    // A removed row keeps its last value.
    expect(window.rows[1].value).toEqual({kind: 'text', text: '0.5'});
  });

  test('sections that appear or disappear', () => {
    const sf: PropSection = {
      title: 'SurfaceFlinger',
      rows: new PropRowsBuilder().text('Z', '1').build(),
    };
    expect(diffSections([sf], [])).toEqual([
      {
        title: 'SurfaceFlinger',
        rows: [
          {
            label: 'Z',
            value: {kind: 'text', text: '1'},
            diff: {kind: 'removed'},
          },
        ],
      },
    ]);
    expect(diffSections([], [sf])[0].rows[0].diff).toEqual({kind: 'added'});
  });

  test('identical sections have no diff', () => {
    const s = sectionsOf(node('1', undefined, 'hi'));
    expect(hasDiff(diffSections(s, s))).toBe(false);
  });

  test('per-frame counters are shown but never marked', () => {
    const sections = (frame: string, recompositions?: string) => [
      {
        title: 'SurfaceFlinger',
        rows: new PropRowsBuilder().text('Frame', frame).build(),
      },
      {
        title: 'Composable',
        rows: new PropRowsBuilder()
          .text('Recompositions', recompositions)
          .build(),
      },
    ];
    const out = diffSections(sections('448'), sections('512', '3'));
    expect(hasDiff(out)).toBe(false);
    expect(out[0].rows[0].value).toEqual({kind: 'text', text: '512'});
    expect(out[1].rows.map((r) => r.label)).toEqual(['Recompositions']);
    // The WM window frame (bounds) is a real change.
    const win = (frame: string) => [
      {
        title: 'Window',
        rows: new PropRowsBuilder().text('Frame', frame).build(),
      },
    ];
    expect(hasDiff(diffSections(win('a'), win('b')))).toBe(true);
  });
});

describe('diffTrees', () => {
  test('added, changed and the roots of removed subtrees', () => {
    const prev = [
      node('1'),
      node('2', '1', 'old'),
      node('3', '1'),
      node('4', '3'),
      node('5', '1'),
    ];
    const cur = [
      node('1'),
      node('2', '1', 'new'),
      node('5', '1'),
      node('6', '5'),
    ];
    const diff = diffTrees(prev, cur, sectionsOf);
    expect([...diff.changes]).toEqual([
      ['2', 'changed'],
      ['6', 'added'],
    ]);
    // 4 went away with its parent 3.
    expect(diff.removed.map((n) => n.nodeId)).toEqual(['3']);
  });

  test('changed nodes with their ancestors', () => {
    const prev = [
      node('1'),
      node('2', '1'),
      node('3', '2', 'a'),
      node('4', '1'),
    ];
    const cur = [node('1'), node('2', '1'), node('3', '2', 'b')];
    const diff = diffTrees(prev, cur, sectionsOf);
    const keep = changedWithAncestors(
      diff,
      new Map(cur.map((n) => [n.nodeId, n])),
    );
    expect([...keep].sort()).toEqual(['1', '2', '3', '4']);
  });
});
