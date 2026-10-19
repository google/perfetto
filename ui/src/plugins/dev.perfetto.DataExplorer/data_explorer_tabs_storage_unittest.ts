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

import {DATA_EXPLORER_PERSISTED_STATE_SCHEMA} from './data_explorer_tabs_storage';

describe('DATA_EXPLORER_PERSISTED_STATE_SCHEMA', () => {
  describe('valid state', () => {
    test('accepts v2 multi-tab state', () => {
      const state = {
        version: 2,
        tabs: [
          {id: 'tab-1', title: 'Graph 1', graphJson: '{}'},
          {id: 'tab-2', title: 'Graph 2'},
        ],
        activeTabId: 'tab-1',
        dashboards: [
          {
            id: 'db-1',
            graphTabId: 'tab-1',
            items: [],
            brushFilters: {node1: []},
          },
        ],
      };
      expect(DATA_EXPLORER_PERSISTED_STATE_SCHEMA.parse(state)).toEqual(state);
    });

    test('accepts v1 single-graph state', () => {
      const state = {version: 1, graphJson: '{}'};
      expect(DATA_EXPLORER_PERSISTED_STATE_SCHEMA.parse(state)).toEqual(state);
    });

    test('accepts state with only a version', () => {
      expect(DATA_EXPLORER_PERSISTED_STATE_SCHEMA.parse({version: 2})).toEqual({
        version: 2,
      });
    });

    test('strips unknown top-level keys', () => {
      const result = DATA_EXPLORER_PERSISTED_STATE_SCHEMA.parse({
        version: 2,
        unexpected: 'value',
      });
      expect(result).toEqual({version: 2});
    });

    test('strips unknown keys inside tabs', () => {
      const result = DATA_EXPLORER_PERSISTED_STATE_SCHEMA.parse({
        version: 2,
        tabs: [{id: 'tab-1', title: 'Graph 1', dashboards: []}],
      });
      expect(result).toEqual({
        version: 2,
        tabs: [{id: 'tab-1', title: 'Graph 1'}],
      });
    });
  });

  describe('invalid state is rejected', () => {
    test('rejects non-string tab title', () => {
      const result = DATA_EXPLORER_PERSISTED_STATE_SCHEMA.safeParse({
        version: 2,
        tabs: [
          {
            id: 'tab-1',
            title: {tag: '<', children: '<b>not a string</b>'},
          },
        ],
        activeTabId: 'tab-1',
      });
      expect(result.success).toBe(false);
    });

    test('rejects non-string tab id', () => {
      const result = DATA_EXPLORER_PERSISTED_STATE_SCHEMA.safeParse({
        version: 2,
        tabs: [{id: 123, title: 'Graph 1'}],
      });
      expect(result.success).toBe(false);
    });

    test('rejects non-string graphJson', () => {
      const result = DATA_EXPLORER_PERSISTED_STATE_SCHEMA.safeParse({
        version: 1,
        graphJson: {nodes: []},
      });
      expect(result.success).toBe(false);
    });

    test('rejects non-array tabs', () => {
      const result = DATA_EXPLORER_PERSISTED_STATE_SCHEMA.safeParse({
        version: 2,
        tabs: {id: 'tab-1', title: 'Graph 1'},
      });
      expect(result.success).toBe(false);
    });

    test('rejects malformed dashboard', () => {
      const result = DATA_EXPLORER_PERSISTED_STATE_SCHEMA.safeParse({
        version: 2,
        dashboards: [{id: 'db-1'}],
      });
      expect(result.success).toBe(false);
    });

    test('rejects unsupported version', () => {
      expect(
        DATA_EXPLORER_PERSISTED_STATE_SCHEMA.safeParse({version: 3}).success,
      ).toBe(false);
      expect(
        DATA_EXPLORER_PERSISTED_STATE_SCHEMA.safeParse({version: '2'}).success,
      ).toBe(false);
    });

    test('rejects missing version', () => {
      expect(
        DATA_EXPLORER_PERSISTED_STATE_SCHEMA.safeParse({graphJson: '{}'})
          .success,
      ).toBe(false);
    });

    test('rejects non-object input', () => {
      expect(
        DATA_EXPLORER_PERSISTED_STATE_SCHEMA.safeParse(undefined).success,
      ).toBe(false);
      expect(DATA_EXPLORER_PERSISTED_STATE_SCHEMA.safeParse(null).success).toBe(
        false,
      );
      expect(
        DATA_EXPLORER_PERSISTED_STATE_SCHEMA.safeParse('state').success,
      ).toBe(false);
      expect(DATA_EXPLORER_PERSISTED_STATE_SCHEMA.safeParse(42).success).toBe(
        false,
      );
      expect(DATA_EXPLORER_PERSISTED_STATE_SCHEMA.safeParse([]).success).toBe(
        false,
      );
    });
  });
});
