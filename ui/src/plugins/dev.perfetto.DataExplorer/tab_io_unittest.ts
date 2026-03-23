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

import {DashboardTabState, DataExplorerTab} from './data_explorer';
import {
  isSerializedTabExport,
  deserializeDashboardsFromExport,
  SerializedTabExport,
} from './graph_io';
import {
  serializeDashboardsForTab,
  SerializedDashboard,
} from './data_explorer_tabs_storage';

describe('isSerializedTabExport', () => {
  test('returns true for valid tab export', () => {
    const obj: SerializedTabExport = {
      version: 1,
      title: 'My Tab',
      graph: '{"nodes":[],"rootNodeIds":[]}',
    };
    expect(isSerializedTabExport(obj)).toBe(true);
  });

  test('returns true for tab export with dashboards', () => {
    const obj: SerializedTabExport = {
      version: 1,
      title: 'My Tab',
      graph: '{}',
      dashboards: [{id: 'd1'}],
    };
    expect(isSerializedTabExport(obj)).toBe(true);
  });

  test('returns true with extra unknown fields', () => {
    const obj = {version: 1, title: 'x', graph: '{}', extra: 'ignored'};
    expect(isSerializedTabExport(obj)).toBe(true);
  });

  test('returns false for null', () => {
    expect(isSerializedTabExport(null)).toBe(false);
  });

  test('returns false for undefined', () => {
    expect(isSerializedTabExport(undefined)).toBe(false);
  });

  test('returns false for primitives', () => {
    expect(isSerializedTabExport(42)).toBe(false);
    expect(isSerializedTabExport('string')).toBe(false);
    expect(isSerializedTabExport(true)).toBe(false);
  });

  test('returns false for arrays', () => {
    expect(isSerializedTabExport([1, 2, 3])).toBe(false);
  });

  test('returns false for empty object', () => {
    expect(isSerializedTabExport({})).toBe(false);
  });

  test('returns false for plain graph export', () => {
    const plainGraph = {nodes: [], rootNodeIds: []};
    expect(isSerializedTabExport(plainGraph)).toBe(false);
  });

  test('returns false for missing graph field', () => {
    expect(isSerializedTabExport({version: 1, title: 'x'})).toBe(false);
  });

  test('returns false for missing title field', () => {
    expect(isSerializedTabExport({version: 1, graph: '{}'})).toBe(false);
  });

  test('returns false for missing version field', () => {
    expect(isSerializedTabExport({title: 'x', graph: '{}'})).toBe(false);
  });

  test('returns false for wrong types', () => {
    expect(isSerializedTabExport({version: 'a', title: 'x', graph: '{}'})).toBe(
      false,
    );
    expect(isSerializedTabExport({version: 1, title: 42, graph: '{}'})).toBe(
      false,
    );
    expect(isSerializedTabExport({version: 1, title: 'x', graph: 123})).toBe(
      false,
    );
  });
});

describe('deserializeDashboardsFromExport', () => {
  test('returns undefined for undefined input', () => {
    expect(deserializeDashboardsFromExport(undefined)).toBeUndefined();
  });

  test('returns undefined for empty array', () => {
    expect(deserializeDashboardsFromExport([])).toBeUndefined();
  });

  // Bug fix: dashboards field could be a non-array type from malformed JSON
  test('returns undefined for non-array input (string)', () => {
    expect(
      deserializeDashboardsFromExport('invalid' as unknown),
    ).toBeUndefined();
  });

  test('returns undefined for non-array input (number)', () => {
    expect(deserializeDashboardsFromExport(42 as unknown)).toBeUndefined();
  });

  test('returns undefined for non-array input (object)', () => {
    expect(
      deserializeDashboardsFromExport({foo: 'bar'} as unknown),
    ).toBeUndefined();
  });

  test('skips entries that are null', () => {
    const result = deserializeDashboardsFromExport([
      null,
      {id: 'db1'},
    ] as unknown as SerializedDashboard[]);
    expect(result?.length).toBe(1);
    expect(result?.[0].id).toBe('db1');
  });

  test('skips entries that are primitives', () => {
    const result = deserializeDashboardsFromExport([
      42,
      'str',
      {id: 'db1'},
    ] as unknown as SerializedDashboard[]);
    expect(result?.length).toBe(1);
    expect(result?.[0].id).toBe('db1');
  });

  test('skips entries with missing id', () => {
    const result = deserializeDashboardsFromExport([
      {noId: true},
      {id: 'db1'},
    ] as unknown as SerializedDashboard[]);
    expect(result?.length).toBe(1);
    expect(result?.[0].id).toBe('db1');
  });

  test('accepts entries with only id', () => {
    const result = deserializeDashboardsFromExport([
      {id: 'db1'},
      {id: 'db2'},
    ] as unknown as SerializedDashboard[]);
    expect(result?.length).toBe(2);
    expect(result?.[0].id).toBe('db1');
    expect(result?.[1].id).toBe('db2');
  });

  test('skips entries with non-string id', () => {
    const result = deserializeDashboardsFromExport([
      {id: 123},
      {id: 'db1'},
    ] as unknown as SerializedDashboard[]);
    expect(result?.length).toBe(1);
    expect(result?.[0].id).toBe('db1');
  });

  test('returns undefined when ALL entries are invalid', () => {
    const result = deserializeDashboardsFromExport([
      null,
      42,
      {noId: true},
      {id: 123},
    ] as unknown as SerializedDashboard[]);
    expect(result).toBeUndefined();
  });

  test('hydrates dashboards with valid label items', () => {
    const serialized: SerializedDashboard[] = [
      {
        id: 'db1',
        items: [{kind: 'label', id: 'lbl1', text: 'Hello', col: 1, row: 2}],
      },
    ];
    const result = deserializeDashboardsFromExport(serialized);
    expect(result?.length).toBe(1);
    expect(result?.[0].id).toBe('db1');
    expect(result?.[0].items.length).toBe(1);
    expect(result?.[0].brushFilters).toEqual(new Map());
  });

  test('filters out invalid items during hydration', () => {
    const serialized: SerializedDashboard[] = [
      {
        id: 'db1',
        items: [
          {kind: 'label', id: 'lbl1', text: 'Good'},
          {kind: 'label'}, // missing id and text
          {kind: 'chart'}, // missing sourceNodeId and config
          {kind: 'unknown_kind', id: 'x', text: 'x'},
          42, // not an object
        ] as unknown[],
      },
    ];
    const result = deserializeDashboardsFromExport(serialized);
    expect(result?.length).toBe(1);
    expect(result?.[0].items.length).toBe(1);
    expect((result?.[0].items[0] as {id: string}).id).toBe('lbl1');
  });

  test('returns empty items when all items are invalid', () => {
    const serialized: SerializedDashboard[] = [
      {
        id: 'db1',
        items: [
          {kind: 'chart'}, // missing required fields
          null,
        ] as unknown[],
      },
    ];
    const result = deserializeDashboardsFromExport(serialized);
    expect(result?.length).toBe(1);
    expect(result?.[0].items).toEqual([]);
  });

  test('hydrates dashboards with brush filters', () => {
    const serialized: SerializedDashboard[] = [
      {
        id: 'db1',
        brushFilters: {
          node1: [{column: 'ts', op: '>=', value: 100}],
        },
      },
    ];
    const result = deserializeDashboardsFromExport(serialized);
    expect(result?.[0].brushFilters.size).toBe(1);
    expect(result?.[0].brushFilters.get('node1')).toEqual([
      {column: 'ts', op: '>=', value: 100},
    ]);
  });

  test('drops brush filters with invalid ops', () => {
    const serialized: SerializedDashboard[] = [
      {
        id: 'db1',
        brushFilters: {
          node1: [
            {column: 'ts', op: '>=', value: 100},
            {column: 'ts', op: 'INVALID', value: 0},
          ],
        },
      },
    ];
    const result = deserializeDashboardsFromExport(serialized);
    expect(result?.[0].brushFilters.get('node1')?.length).toBe(1);
    expect(result?.[0].brushFilters.get('node1')?.[0].op).toBe('>=');
  });

  test('drops brush filters with missing column', () => {
    const serialized: SerializedDashboard[] = [
      {
        id: 'db1',
        brushFilters: {
          node1: [{op: '>=', value: 100}],
        } as unknown as Record<string, unknown[]>,
      },
    ];
    const result = deserializeDashboardsFromExport(serialized);
    // All filters for node1 are invalid, so the entry is dropped entirely
    expect(result?.[0].brushFilters.size).toBe(0);
  });

  test('ignores non-object brushFilters (array)', () => {
    const serialized = [
      {
        id: 'db1',
        brushFilters: [{column: 'ts', op: '>='}],
      },
    ] as unknown as SerializedDashboard[];
    const result = deserializeDashboardsFromExport(serialized);
    // brushFilters is an array, not a record — should fall back to empty map
    expect(result?.[0].brushFilters).toEqual(new Map());
  });

  test('ignores non-object brushFilters (string)', () => {
    const serialized = [
      {
        id: 'db1',
        brushFilters: 'invalid',
      },
    ] as unknown as SerializedDashboard[];
    const result = deserializeDashboardsFromExport(serialized);
    expect(result?.[0].brushFilters).toEqual(new Map());
  });

  test('handles dashboards with no items field', () => {
    const serialized: SerializedDashboard[] = [{id: 'db1'}];
    const result = deserializeDashboardsFromExport(serialized);
    expect(result?.[0].items).toEqual([]);
  });

  test('hydrates multiple dashboards', () => {
    const serialized: SerializedDashboard[] = [{id: 'db1'}, {id: 'db2'}];
    const result = deserializeDashboardsFromExport(serialized);
    expect(result?.length).toBe(2);
    expect(result?.[0].id).toBe('db1');
    expect(result?.[1].id).toBe('db2');
  });

  test('supports is null brush filter (no value field)', () => {
    const serialized: SerializedDashboard[] = [
      {
        id: 'db1',
        brushFilters: {
          node1: [{column: 'name', op: 'is null'}],
        },
      },
    ];
    const result = deserializeDashboardsFromExport(serialized);
    expect(result?.[0].brushFilters.get('node1')).toEqual([
      {column: 'name', op: 'is null'},
    ]);
  });
});

describe('serializeDashboardsForTab', () => {
  function makeTab(dashboards: DashboardTabState[]): DataExplorerTab {
    return {
      id: 'tab1',
      title: 'Test Tab',
      state: {
        rootNodes: [],
        selectedNodes: new Set(),
        nodeLayouts: new Map(),
        labels: [],
      },
      dashboards,
    };
  }

  test('returns undefined for tab with no dashboards', () => {
    const tab = makeTab([]);
    expect(serializeDashboardsForTab(tab)).toBeUndefined();
  });

  test('serializes tab with empty dashboard items', () => {
    const tab = makeTab([{id: 'db1', items: [], brushFilters: new Map()}]);
    const result = serializeDashboardsForTab(tab);
    expect(result).toBeDefined();
    expect(result?.length).toBe(1);
    expect(result?.[0].items).toBeUndefined();
    expect(result?.[0].brushFilters).toBeUndefined();
  });

  test('serializes dashboard with label items', () => {
    const tab = makeTab([
      {
        id: 'db1',
        items: [{kind: 'label', id: 'lbl1', text: 'Hello', col: 0, row: 0}],
        brushFilters: new Map(),
      },
    ]);
    const result = serializeDashboardsForTab(tab);
    expect(result?.length).toBe(1);
    expect(result?.[0].id).toBe('db1');
    expect(result?.[0].items?.length).toBe(1);
    expect(result?.[0].brushFilters).toBeUndefined();
  });

  test('serializes brush filters with BigInt conversion', () => {
    const filters = new Map([
      ['node1', [{column: 'ts', op: '>=' as const, value: BigInt(100)}]],
    ]);
    const tab = makeTab([
      {
        id: 'db1',
        items: [{kind: 'label', id: 'lbl1', text: 'Hello', col: 0, row: 0}],
        brushFilters: filters,
      },
    ]);
    const result = serializeDashboardsForTab(tab);
    expect(result?.[0].brushFilters).toBeDefined();
    expect(result?.[0].brushFilters?.['node1']?.[0]).toEqual({
      column: 'ts',
      op: '>=',
      value: 100,
    });
  });

  test('serializes multiple dashboards', () => {
    const tab = makeTab([
      {
        id: 'db1',
        items: [{kind: 'label', id: 'l1', text: 'A', col: 0, row: 0}],
        brushFilters: new Map(),
      },
      {
        id: 'db2',
        items: [{kind: 'label', id: 'l2', text: 'B', col: 1, row: 1}],
        brushFilters: new Map(),
      },
    ]);
    const result = serializeDashboardsForTab(tab);
    expect(result?.length).toBe(2);
    expect(result?.[0].id).toBe('db1');
    expect(result?.[1].id).toBe('db2');
  });

  test('does not include graphTabId in output', () => {
    const tab = makeTab([
      {
        id: 'db1',
        items: [{kind: 'label', id: 'l1', text: 'x', col: 0, row: 0}],
        brushFilters: new Map(),
      },
    ]);
    const result = serializeDashboardsForTab(tab);
    expect(result?.[0]).not.toHaveProperty('graphTabId');
  });

  test('serializes brush filters for is null op without value', () => {
    const filters = new Map([
      ['node1', [{column: 'name', op: 'is null' as const}]],
    ]);
    const tab = makeTab([
      {
        id: 'db1',
        items: [{kind: 'label', id: 'l1', text: 'x', col: 0, row: 0}],
        brushFilters: filters,
      },
    ]);
    const result = serializeDashboardsForTab(tab);
    expect(result?.[0].brushFilters?.['node1']?.[0]).toEqual({
      column: 'name',
      op: 'is null',
    });
  });
});

describe('tab export/import round-trip', () => {
  test('hydrated dashboards match original structure', () => {
    const original: DashboardTabState[] = [
      {
        id: 'db1',
        items: [
          {kind: 'label', id: 'lbl1', text: 'Test Label', col: 1, row: 2},
        ],
        brushFilters: new Map([
          ['node1', [{column: 'dur', op: '>=' as const, value: 500}]],
        ]),
      },
      {
        id: 'db2',
        items: [],
        brushFilters: new Map(),
      },
    ];

    const tab: DataExplorerTab = {
      id: 'tab1',
      title: 'Test Tab',
      state: {
        rootNodes: [],
        selectedNodes: new Set(),
        nodeLayouts: new Map(),
        labels: [],
      },
      dashboards: original,
    };

    // Serialize
    const serialized = serializeDashboardsForTab(tab);
    expect(serialized).toBeDefined();

    // Hydrate back
    const hydrated = deserializeDashboardsFromExport(serialized);
    expect(hydrated).toBeDefined();

    // Both dashboards get serialized (even the empty one)
    expect(hydrated?.length).toBe(2);

    // First dashboard: has items and brush filters
    expect(hydrated?.[0].id).toBe('db1');
    expect(hydrated?.[0].items.length).toBe(1);
    expect(hydrated?.[0].brushFilters.size).toBe(1);
    expect(hydrated?.[0].brushFilters.get('node1')).toEqual([
      {column: 'dur', op: '>=', value: 500},
    ]);

    // Second dashboard: empty items, no filters
    expect(hydrated?.[1].id).toBe('db2');
    expect(hydrated?.[1].items).toEqual([]);
    expect(hydrated?.[1].brushFilters).toEqual(new Map());
  });

  test('round-trip through JSON.stringify/parse preserves data', () => {
    const tab: DataExplorerTab = {
      id: 'tab1',
      title: 'Test Tab',
      state: {
        rootNodes: [],
        selectedNodes: new Set(),
        nodeLayouts: new Map(),
        labels: [],
      },
      dashboards: [
        {
          id: 'db1',
          items: [{kind: 'label', id: 'lbl1', text: 'Label', col: 1, row: 1}],
          brushFilters: new Map([
            ['n1', [{column: 'ts', op: '>=' as const, value: 1000}]],
          ]),
        },
      ],
    };

    const serialized = serializeDashboardsForTab(tab);
    // Simulate going through JSON file (as exportTab does)
    const jsonString = JSON.stringify(serialized);
    const parsed = JSON.parse(jsonString) as SerializedDashboard[];

    const hydrated = deserializeDashboardsFromExport(parsed);
    expect(hydrated?.length).toBe(1);
    expect(hydrated?.[0].id).toBe('db1');
    expect(hydrated?.[0].items.length).toBe(1);
    expect(hydrated?.[0].brushFilters.get('n1')).toEqual([
      {column: 'ts', op: '>=', value: 1000},
    ]);
  });
});
