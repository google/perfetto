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

import {DEFAULT_HDE_STATE, HDE_STATE_SCHEMA} from './persisted_state';

const TREE_STATE = {
  selectedMetricId: 'self',
  addedMetricIds: ['total'],
  displayMode: 'tree',
  filters: [{kind: 'SHOW_STACK', filter: 'foo'}],
  view: {kind: 'TOP_DOWN'},
};

describe('HDE_STATE_SCHEMA', () => {
  test('accepts the default state', () => {
    expect(HDE_STATE_SCHEMA.parse(DEFAULT_HDE_STATE)).toEqual({});
  });

  test('accepts a fully populated state', () => {
    const state = {
      activeDump: {upid: 3, ts: '123456789012345'},
      nav: 'flamegraph',
      flamegraphTabs: [{pathHashes: 'abc', isDominator: true}],
      instanceTabs: [{objId: 42, label: 'java.lang.String'}],
      flamegraphPanelState: TREE_STATE,
      callstackPanelState: TREE_STATE,
    };
    expect(HDE_STATE_SCHEMA.parse(state)).toEqual(state);
  });

  test('rejects a numeric dump timestamp', () => {
    const result = HDE_STATE_SCHEMA.safeParse({
      activeDump: {upid: 3, ts: 123},
    });
    expect(result.success).toBe(false);
  });

  test('rejects malformed tabs', () => {
    const result = HDE_STATE_SCHEMA.safeParse({
      instanceTabs: [{objId: '42'}],
    });
    expect(result.success).toBe(false);
  });

  test('rejects undefined (relies on defaultValue)', () => {
    expect(HDE_STATE_SCHEMA.safeParse(undefined).success).toBe(false);
  });
});
