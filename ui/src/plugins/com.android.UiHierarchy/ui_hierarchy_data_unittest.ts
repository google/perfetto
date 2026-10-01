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
  computeVersionDiffs,
  kindToString,
  type UiHierarchyNode,
} from './ui_hierarchy_data';

describe('UiHierarchyData', () => {
  describe('kindToString', () => {
    it('correctly maps integer kinds to kind strings', () => {
      expect(kindToString(1)).toBe('View');
      expect(kindToString(2)).toBe('ComposeView');
      expect(kindToString(3)).toBe('ComposeNode');
      expect(kindToString(4)).toBe('Composable');
      expect(kindToString(99)).toBe('Unknown');
    });
  });

  describe('computeVersionDiffs', () => {
    const baseNode: UiHierarchyNode = {
      rowId: 1,
      ts: 1000n,
      dur: 100n,
      windowId: 10,
      nodeId: '42',
      childIndex: 0,
      kind: 3,
      kindName: 'ComposeNode',
      name: 'CounterButton',
      boundsLeft: 0,
      boundsTop: 0,
      boundsRight: 100,
      boundsBottom: 50,
      alpha: 1.0,
      flags: 5n,
      isVisible: true,
      isClickable: true,
      isFocused: false,
      isTextRedacted: false,
      text: 'Count: 0',
      recompositionCount: 0,
    };

    it('handles initial version', () => {
      const diffs = computeVersionDiffs([baseNode]);
      expect(diffs.length).toBe(1);
      expect(diffs[0].versionIndex).toBe(0);
      expect(diffs[0].ts).toBe(1000n);
      expect(diffs[0].changedProps).toContain('Initial version');
    });

    it('detects text changes between versions', () => {
      const v2: UiHierarchyNode = {
        ...baseNode,
        rowId: 2,
        ts: 2000n,
        text: 'Count: 1',
        recompositionCount: 1,
      };
      const diffs = computeVersionDiffs([baseNode, v2]);
      expect(diffs.length).toBe(2);
      expect(diffs[1].changedProps).toContain('text: "Count: 0" → "Count: 1"');
      expect(diffs[1].changedProps).toContain('recompositions: 0 → 1');
    });

    it('detects bounds and visibility changes', () => {
      const v2: UiHierarchyNode = {
        ...baseNode,
        rowId: 2,
        ts: 2000n,
        boundsRight: 200,
        isVisible: false,
        flags: 4n,
      };
      const diffs = computeVersionDiffs([baseNode, v2]);
      expect(diffs.length).toBe(2);
      expect(diffs[1].changedProps).toContain(
        'bounds: [0,0,100,50] → [0,0,200,50]',
      );
      expect(diffs[1].changedProps).toContain('visible: true → false');
    });
  });
});
