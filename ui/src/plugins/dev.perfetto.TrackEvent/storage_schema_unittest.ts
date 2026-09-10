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

import {TRACK_EVENT_PLUGIN_STATE_SCHEMA} from './index';

const TREE_STATE = {
  selectedMetricId: 'self',
  addedMetricIds: ['total'],
  displayMode: 'tree',
  filters: [{kind: 'SHOW_STACK', filter: 'foo'}],
  view: {kind: 'TOP_DOWN'},
};

describe('TRACK_EVENT_PLUGIN_STATE_SCHEMA', () => {
  test('accepts empty state', () => {
    expect(TRACK_EVENT_PLUGIN_STATE_SCHEMA.parse({})).toEqual({});
  });

  test('accepts a populated state', () => {
    const state = {areaSelectionFlamegraphState: TREE_STATE};
    expect(TRACK_EVENT_PLUGIN_STATE_SCHEMA.parse(state)).toEqual(state);
  });

  test('fills defaults in nested tree explorer state', () => {
    const result = TRACK_EVENT_PLUGIN_STATE_SCHEMA.parse({
      areaSelectionFlamegraphState: {
        selectedMetricId: 'self',
        filters: [],
        view: {kind: 'BOTTOM_UP'},
      },
    });
    expect(result.areaSelectionFlamegraphState).toEqual({
      selectedMetricId: 'self',
      addedMetricIds: [],
      displayMode: 'flamegraph',
      filters: [],
      view: {kind: 'BOTTOM_UP'},
    });
  });

  test('rejects an invalid tree explorer state', () => {
    const result = TRACK_EVENT_PLUGIN_STATE_SCHEMA.safeParse({
      areaSelectionFlamegraphState: {...TREE_STATE, view: {kind: 'NOT_A_VIEW'}},
    });
    expect(result.success).toBe(false);
  });

  test('rejects undefined (relies on defaultValue)', () => {
    expect(TRACK_EVENT_PLUGIN_STATE_SCHEMA.safeParse(undefined).success).toBe(
      false,
    );
  });
});
