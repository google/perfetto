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

// Unit tests for the slice of Heap Dump Explorer state that survives in a
// shared permalink. This is the contract the core (de)serializes; a break here
// silently corrupts shared links, so the round-trip is pinned explicitly.

import {migrateHdeState, type HdeState} from './persisted_state';

// A permalink survives a JSON encode/decode in the core, so round-tripping
// through JSON is the realistic fidelity check.
function roundTrip(state: HdeState): HdeState {
  return migrateHdeState(JSON.parse(JSON.stringify(state)));
}

describe('migrateHdeState', () => {
  it('round-trips a full state including diff-mode object tabs', () => {
    const state: HdeState = {
      activeDump: {upid: 3, ts: '123456789'},
      nav: 'object_0x2a',
      flamegraphTabs: [
        {pathHashes: 'a,b,c', isDominator: false},
        {pathHashes: 'd', isDominator: true},
      ],
      // A diff-mode tab pairs a current-side and baseline-side object id.
      instanceTabs: [
        {objId: 42, label: 'Foo', currentId: 42, baselineId: 7},
        {objId: 99, label: 'OnlyCurrent', currentId: 99, baselineId: null},
      ],
      flamegraphPanelState: {
        selectedMetricName: 'Object Size',
        filters: [],
        view: {kind: 'TOP_DOWN'},
      },
    };
    expect(roundTrip(state)).toEqual(state);
  });

  it('keeps a legacy permalink without currentId/baselineId parseable', () => {
    // Permalinks predating the diff feature stored only {objId, label}.
    const legacy = {
      activeDump: {upid: 1, ts: '10'},
      instanceTabs: [{objId: 5, label: 'Legacy'}],
    };
    const migrated = migrateHdeState(legacy);
    expect(migrated.instanceTabs).toEqual([{objId: 5, label: 'Legacy'}]);
  });

  it('falls back to empty state on unparseable input rather than throwing', () => {
    expect(migrateHdeState(undefined)).toEqual({});
    expect(migrateHdeState('not an object')).toEqual({});
    expect(migrateHdeState({activeDump: {upid: 'nope'}})).toEqual({});
  });
});
