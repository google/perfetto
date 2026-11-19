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

import {STACK_SAMPLES_PLUGIN_STATE_SCHEMA} from './index';

const TREE_STATE = {
  selectedMetricId: 'self',
  addedMetricIds: ['total'],
  displayMode: 'tree',
  filters: [{kind: 'SHOW_STACK', filter: 'foo'}],
  view: {kind: 'TOP_DOWN'},
};

describe('STACK_SAMPLES_PLUGIN_STATE_SCHEMA', () => {
  test('accepts empty state', () => {
    expect(STACK_SAMPLES_PLUGIN_STATE_SCHEMA.parse({})).toEqual({});
  });

  test('accepts per-source flamegraph states', () => {
    const state = {
      areaSelectionFlamegraphStates: {linux_perf: TREE_STATE},
      detailsPanelFlamegraphStates: {
        linux_perf: TREE_STATE,
        simpleperf: TREE_STATE,
      },
    };
    expect(STACK_SAMPLES_PLUGIN_STATE_SCHEMA.parse(state)).toEqual(state);
  });

  test('rejects an invalid per-source tree explorer state', () => {
    const result = STACK_SAMPLES_PLUGIN_STATE_SCHEMA.safeParse({
      detailsPanelFlamegraphStates: {linux_perf: {selectedMetricId: 1}},
    });
    expect(result.success).toBe(false);
  });

  test('rejects undefined (relies on defaultValue)', () => {
    expect(STACK_SAMPLES_PLUGIN_STATE_SCHEMA.safeParse(undefined).success).toBe(
      false,
    );
  });
});
