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

import {describe, expect, test} from 'vitest';
import {
  TREE_EXPLORER_STATE_SCHEMA,
  createDefaultTreeExplorerState,
  updateTreeExplorerState,
} from './tree_explorer';
import {getTreeExplorerMergeSelection} from './tree_explorer_merge';

describe('TreeExplorerState.merge', () => {
  test('defaults to the merged tree', () => {
    const state = createDefaultTreeExplorerState([{name: 'cpu', unit: 'ns'}]);
    expect(getTreeExplorerMergeSelection(state)).toEqual({merged: true});
  });

  test('state persisted before merging existed still parses', () => {
    const state = TREE_EXPLORER_STATE_SCHEMA.parse({
      selectedMetricId: 'cpu',
      filters: [],
      view: {kind: 'TOP_DOWN'},
    });
    expect(getTreeExplorerMergeSelection(state)).toEqual({merged: true});
  });

  test('round-trips through the schema', () => {
    const state = TREE_EXPLORER_STATE_SCHEMA.parse({
      selectedMetricId: 'cpu',
      filters: [],
      view: {kind: 'TOP_DOWN'},
      merge: {merged: false, profileKey: 'b'},
    });
    expect(getTreeExplorerMergeSelection(state)).toEqual({
      merged: false,
      profileKey: 'b',
    });
  });

  test('survives a change of the selected metric', () => {
    const state = {
      ...createDefaultTreeExplorerState([{name: 'allocations', unit: ''}]),
      merge: {merged: false, profileKey: 'b'},
    };
    const updated = updateTreeExplorerState(state, [{name: 'cpu', unit: 'ns'}]);
    expect(updated.selectedMetricId).toBe('cpu');
    expect(updated.merge).toEqual({merged: false, profileKey: 'b'});
  });
});
