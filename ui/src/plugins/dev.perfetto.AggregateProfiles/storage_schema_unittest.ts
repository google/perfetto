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

import {AGGREGATE_PROFILES_PAGE_STATE_SCHEMA} from './types';

const TREE_STATE = {
  selectedMetricId: 'self',
  addedMetricIds: ['total'],
  displayMode: 'tree',
  filters: [{kind: 'SHOW_STACK', filter: 'foo'}],
  view: {kind: 'TOP_DOWN'},
};

describe('AGGREGATE_PROFILES_PAGE_STATE_SCHEMA', () => {
  test('accepts empty state', () => {
    expect(AGGREGATE_PROFILES_PAGE_STATE_SCHEMA.parse({})).toEqual({});
  });

  test('accepts a populated state', () => {
    const state = {flamegraphState: TREE_STATE};
    expect(AGGREGATE_PROFILES_PAGE_STATE_SCHEMA.parse(state)).toEqual(state);
  });

  test('fills defaults in nested tree explorer state', () => {
    const result = AGGREGATE_PROFILES_PAGE_STATE_SCHEMA.parse({
      flamegraphState: {
        selectedMetricId: 'self',
        filters: [],
        view: {kind: 'BOTTOM_UP'},
      },
    });
    expect(result.flamegraphState).toEqual({
      selectedMetricId: 'self',
      addedMetricIds: [],
      displayMode: 'flamegraph',
      filters: [],
      view: {kind: 'BOTTOM_UP'},
    });
  });

  test('rejects an invalid tree explorer state', () => {
    const result = AGGREGATE_PROFILES_PAGE_STATE_SCHEMA.safeParse({
      flamegraphState: {...TREE_STATE, view: {kind: 'NOT_A_VIEW'}},
    });
    expect(result.success).toBe(false);
  });

  test('rejects undefined (relies on defaultValue)', () => {
    expect(
      AGGREGATE_PROFILES_PAGE_STATE_SCHEMA.safeParse(undefined).success,
    ).toBe(false);
  });

  test('accepts a selected profile id', () => {
    const state = {selectedProfileId: 'profile-1', flamegraphState: TREE_STATE};
    expect(AGGREGATE_PROFILES_PAGE_STATE_SCHEMA.parse(state)).toEqual(state);
  });

  test('rejects a non-string selected profile id', () => {
    const result = AGGREGATE_PROFILES_PAGE_STATE_SCHEMA.safeParse({
      selectedProfileId: 42,
    });
    expect(result.success).toBe(false);
  });
});
