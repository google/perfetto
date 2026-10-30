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

import {ProfileType} from './common';
import {HEAP_PROFILE_PLUGIN_STATE_SCHEMA} from './index';

const EMPTY_STATE = {
  [ProfileType.NATIVE_HEAP_PROFILE]: {},
  [ProfileType.GENERIC_HEAP_PROFILE]: {},
  [ProfileType.JAVA_HEAP_SAMPLES]: {},
  [ProfileType.JAVA_HEAP_GRAPH]: {},
  [ProfileType.OOME_CALLSTACK]: {},
};

const TREE_STATE = {
  selectedMetricId: 'self',
  addedMetricIds: ['total'],
  displayMode: 'tree',
  filters: [{kind: 'SHOW_STACK', filter: 'foo'}],
  view: {kind: 'TOP_DOWN'},
};

describe('HEAP_PROFILE_PLUGIN_STATE_SCHEMA', () => {
  test('accepts the registered default value', () => {
    expect(HEAP_PROFILE_PLUGIN_STATE_SCHEMA.parse(EMPTY_STATE)).toEqual(
      EMPTY_STATE,
    );
  });

  test('accepts populated flamegraph states', () => {
    const state = {
      ...EMPTY_STATE,
      [ProfileType.NATIVE_HEAP_PROFILE]: {
        trackFlamegraphState: TREE_STATE,
        areaSelectionFlamegraphState: TREE_STATE,
      },
    };
    expect(HEAP_PROFILE_PLUGIN_STATE_SCHEMA.parse(state)).toEqual(state);
  });

  test('accepts state round-tripped through JSON (string keys)', () => {
    const state = {
      ...EMPTY_STATE,
      [ProfileType.JAVA_HEAP_GRAPH]: {trackFlamegraphState: TREE_STATE},
    };
    const raw = JSON.parse(JSON.stringify(state));
    expect(HEAP_PROFILE_PLUGIN_STATE_SCHEMA.parse(raw)).toEqual(state);
  });

  test('rejects state missing a profile type', () => {
    const result = HEAP_PROFILE_PLUGIN_STATE_SCHEMA.safeParse({
      [ProfileType.NATIVE_HEAP_PROFILE]: {},
    });
    expect(result.success).toBe(false);
  });

  test('rejects an invalid tree explorer state', () => {
    const result = HEAP_PROFILE_PLUGIN_STATE_SCHEMA.safeParse({
      ...EMPTY_STATE,
      [ProfileType.NATIVE_HEAP_PROFILE]: {
        trackFlamegraphState: {...TREE_STATE, filters: 'nope'},
      },
    });
    expect(result.success).toBe(false);
  });
});
