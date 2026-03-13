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

import {PerfettoSqlType} from '../../../trace_processor/perfetto_sql_type';
import {
  DashboardDataSource,
  DashboardItem,
  dashboardRegistry,
  getItemId,
  getNextItemPosition,
  parseBrushFilters,
  serializeDashboardData,
  snapToGrid,
  validateDashboardItems,
  sourceDisplayName,
  DashboardSourceWithName,
} from './dashboard_registry';

let nextChartId = 0;
function testChartId(): string {
  return `test-chart-${nextChartId++}`;
}

function makeSource(
  nodeId: string,
  name: string,
  columns: Array<{name: string; type?: PerfettoSqlType}> = [
    {name: 'id', type: {kind: 'id', source: {table: 'test', column: 'id'}}},
  ],
  graphId = 'test-graph',
): DashboardDataSource {
  return {nodeId, name, columns, graphId};
}

function makeSourceWithName(
  nodeId: string,
  name: string,
  graphId: string,
  graphName: string,
): DashboardSourceWithName {
  return {
    nodeId,
    name,
    columns: [{name: 'id', type: {kind: 'int'}}],
    graphId,
    graphName,
  };
}

function makeChartItem(sourceNodeId: string, chartId?: string): DashboardItem {
  return {
    kind: 'chart',
    sourceNodeId,
    config: {
      id: chartId ?? testChartId(),
      column: 'id',
      chartType: 'bar',
    },
  };
}

// --- Exported sources (the only state remaining in the registry) ---

describe('ExportedSourcesPool', () => {
  beforeEach(() => {
    dashboardRegistry.clear();
  });

  test('set and get exported source', () => {
    const source = makeSource('n1', 'Source A');
    dashboardRegistry.setExportedSource(source);
    expect(dashboardRegistry.getExportedSource('n1')).toBe(source);
  });

  test('getAllExportedSources returns all', () => {
    dashboardRegistry.setExportedSource(makeSource('n1', 'A'));
    dashboardRegistry.setExportedSource(makeSource('n2', 'B'));
    expect(dashboardRegistry.getAllExportedSources()).toHaveLength(2);
  });

  test('removeExportedSource removes source', () => {
    dashboardRegistry.setExportedSource(makeSource('n1', 'A'));
    dashboardRegistry.removeExportedSource('n1');
    expect(dashboardRegistry.getExportedSource('n1')).toBeUndefined();
    expect(dashboardRegistry.getAllExportedSources()).toHaveLength(0);
  });

  test('setExportedSource overwrites existing', () => {
    dashboardRegistry.setExportedSource(makeSource('n1', 'Old'));
    dashboardRegistry.setExportedSource(makeSource('n1', 'New'));
    expect(dashboardRegistry.getExportedSource('n1')?.name).toBe('New');
    expect(dashboardRegistry.getAllExportedSources()).toHaveLength(1);
  });

  test('removeExportedSource on non-existent is a no-op', () => {
    dashboardRegistry.removeExportedSource('non-existent');
    // No error thrown.
  });

  test('clear removes all sources', () => {
    dashboardRegistry.setExportedSource(makeSource('n1', 'A'));
    dashboardRegistry.setExportedSource(makeSource('n2', 'B'));
    dashboardRegistry.clear();
    expect(dashboardRegistry.getAllExportedSources()).toHaveLength(0);
  });
});

// --- Utility functions ---

describe('getItemId', () => {
  test('returns config.id for chart items', () => {
    const chart = makeChartItem('n1', 'chart-123');
    expect(getItemId(chart)).toBe('chart-123');
  });
});

describe('snapToGrid', () => {
  test('snaps to nearest 20px', () => {
    expect(snapToGrid(0)).toBe(0);
    expect(snapToGrid(10)).toBe(20);
    expect(snapToGrid(19)).toBe(20);
    expect(snapToGrid(20)).toBe(20);
    expect(snapToGrid(29)).toBe(20);
    expect(snapToGrid(30)).toBe(40);
    expect(snapToGrid(31)).toBe(40);
  });

  test('handles negative values', () => {
    expect(snapToGrid(-10)).toBe(-0); // Math.round(-0.5) = -0
    expect(snapToGrid(-20)).toBe(-20);
    expect(snapToGrid(-31)).toBe(-40);
  });
});

describe('getNextItemPosition', () => {
  test('returns offset based on item count', () => {
    const pos0 = getNextItemPosition([]);
    expect(pos0.x).toBe(20);
    expect(pos0.y).toBe(20);

    const pos1 = getNextItemPosition([makeChartItem('n1')]);
    expect(pos1.x).toBe(60);
    expect(pos1.y).toBe(60);
  });

  test('wraps after 10 items', () => {
    const tenItems = Array.from({length: 10}, () => makeChartItem('n1'));
    const pos = getNextItemPosition(tenItems);
    // 10 % 10 = 0, same as empty
    expect(pos.x).toBe(20);
    expect(pos.y).toBe(20);
  });
});

// --- Serialization ---

describe('serializeDashboardData', () => {
  test('returns empty object for undefined dashboardId', () => {
    expect(serializeDashboardData(undefined)).toEqual({});
  });

  test('returns dashboardId with no items', () => {
    const data = serializeDashboardData('db-1');
    expect(data.dashboardId).toBe('db-1');
    expect(data.dashboardItems).toBeUndefined();
    expect(data.brushFilters).toBeUndefined();
  });

  test('serializes items when present', () => {
    const chart = makeChartItem('n1');
    const data = serializeDashboardData('db-1', [chart]);
    expect(data.dashboardId).toBe('db-1');
    expect(data.dashboardItems).toHaveLength(1);
  });

  test('serializes brush filters when present', () => {
    const filters = new Map([
      ['n1', [{column: 'x', op: '=' as const, value: 42}]],
    ]);
    const data = serializeDashboardData('db-1', [], filters);
    expect(data.brushFilters).toEqual({
      n1: [{column: 'x', op: '=', value: 42}],
    });
  });

  test('omits empty items and filters', () => {
    const data = serializeDashboardData('db-1', [], new Map());
    expect(data.dashboardItems).toBeUndefined();
    expect(data.brushFilters).toBeUndefined();
  });

  test('converts bigint values in filters to numbers', () => {
    const filters = new Map([
      ['n1', [{column: 'x', op: '=' as const, value: BigInt(42)}]],
    ]);
    const data = serializeDashboardData('db-1', [], filters);
    expect(data.brushFilters).toEqual({
      n1: [{column: 'x', op: '=', value: 42}],
    });
  });
});

// --- Validation ---

describe('validateDashboardItems', () => {
  test('returns undefined for undefined input', () => {
    expect(validateDashboardItems(undefined)).toBeUndefined();
  });

  test('returns undefined for empty array', () => {
    expect(validateDashboardItems([])).toBeUndefined();
  });

  test('validates chart items', () => {
    const items = [
      {
        kind: 'chart',
        sourceNodeId: 'n1',
        config: {id: 'c1', column: 'x', chartType: 'bar'},
      },
    ];
    const result = validateDashboardItems(items);
    expect(result).toHaveLength(1);
    expect(result?.[0].kind).toBe('chart');
  });

  test('rejects chart without sourceNodeId', () => {
    const items = [
      {kind: 'chart', config: {id: 'c1', column: 'x', chartType: 'bar'}},
    ];
    expect(validateDashboardItems(items)).toBeUndefined();
  });

  test('rejects chart without config', () => {
    const items = [{kind: 'chart', sourceNodeId: 'n1'}];
    expect(validateDashboardItems(items)).toBeUndefined();
  });

  test('rejects chart with null config', () => {
    const items = [{kind: 'chart', sourceNodeId: 'n1', config: null}];
    expect(validateDashboardItems(items)).toBeUndefined();
  });

  test('skips non-object items', () => {
    const items = [
      'string',
      null,
      42,
      {
        kind: 'chart',
        sourceNodeId: 'n1',
        config: {id: 'c1', column: 'x', chartType: 'bar'},
      },
    ];
    const result = validateDashboardItems(items);
    expect(result).toHaveLength(1);
  });

  test('skips unknown kinds', () => {
    const items = [{kind: 'unknown', id: 'u1'}];
    expect(validateDashboardItems(items)).toBeUndefined();
  });

  test('filters mix of valid and invalid', () => {
    const items = [
      {
        kind: 'chart',
        sourceNodeId: 'n1',
        config: {id: 'c1', column: 'x', chartType: 'bar'},
      },
      {kind: 'chart', sourceNodeId: 'n2'}, // missing config
    ];
    const result = validateDashboardItems(items);
    expect(result).toHaveLength(1);
  });
});

// --- parseBrushFilters ---

describe('parseBrushFilters', () => {
  test('parses valid filters into a Map', () => {
    const raw = {
      n1: [{column: 'x', op: '=', value: 42}],
      n2: [{column: 'y', op: '>=', value: 10}],
    };
    const result = parseBrushFilters(raw);
    expect(result.size).toBe(2);
    expect(result.get('n1')).toEqual([{column: 'x', op: '=', value: 42}]);
    expect(result.get('n2')).toEqual([{column: 'y', op: '>=', value: 10}]);
  });

  test('drops invalid entries', () => {
    const raw = {
      n1: [
        {column: 'x', op: '=', value: 1}, // valid
        {op: '=', value: 2}, // missing column
        {column: 'y', op: 'LIKE', value: 'foo'}, // invalid op
        null, // non-object
        'string', // non-object
      ],
    };
    const result = parseBrushFilters(raw as Record<string, unknown[]>);
    expect(result.size).toBe(1);
    expect(result.get('n1')).toHaveLength(1);
  });

  test('omits sources with no valid filters', () => {
    const raw = {
      n1: [{op: '=', value: 1}], // all invalid — missing column
    };
    const result = parseBrushFilters(raw as Record<string, unknown[]>);
    expect(result.size).toBe(0);
  });

  test('accepts all valid operators', () => {
    const raw = {
      n1: [
        {column: 'a', op: '=', value: 1},
        {column: 'b', op: '>=', value: 2},
        {column: 'c', op: '<', value: 3},
        {column: 'd', op: 'is null'},
      ],
    };
    const result = parseBrushFilters(raw);
    expect(result.get('n1')).toHaveLength(4);
  });

  test('returns empty map for empty input', () => {
    const result = parseBrushFilters({});
    expect(result.size).toBe(0);
  });
});

// --- sourceDisplayName ---

describe('sourceDisplayName', () => {
  test('returns plain name when all charts use one graph', () => {
    const s1 = makeSourceWithName('n1', 'thread_state', 'g1', 'Graph 1');
    const s2 = makeSourceWithName('n2', 'sched', 'g1', 'Graph 1');
    const items = [makeChartItem('n1'), makeChartItem('n2')];
    const sources = [s1, s2];
    expect(sourceDisplayName(s1, items, sources)).toBe('thread_state');
    expect(sourceDisplayName(s2, items, sources)).toBe('sched');
  });

  test('appends graph name when charts use multiple graphs', () => {
    const s1 = makeSourceWithName('n1', 'thread_state', 'g1', 'Graph 1');
    const s2 = makeSourceWithName('n2', 'thread_state', 'g2', 'Graph 2');
    const items = [makeChartItem('n1'), makeChartItem('n2')];
    const sources = [s1, s2];
    expect(sourceDisplayName(s1, items, sources)).toBe(
      'thread_state · Graph 1',
    );
    expect(sourceDisplayName(s2, items, sources)).toBe(
      'thread_state · Graph 2',
    );
  });

  test('no namespacing when only one graph is used even if others available', () => {
    const s1 = makeSourceWithName('n1', 'A', 'g1', 'Graph 1');
    const s2 = makeSourceWithName('n2', 'B', 'g2', 'Graph 2');
    // Only s1 is used in charts; s2 is available but unused.
    const items = [makeChartItem('n1')];
    expect(sourceDisplayName(s1, items, [s1, s2])).toBe('A');
  });

  test('namespacing when two graphs are used even with different names', () => {
    const s1 = makeSourceWithName('n1', 'A', 'g1', 'Graph 1');
    const s2 = makeSourceWithName('n2', 'B', 'g2', 'Graph 2');
    const items = [makeChartItem('n1'), makeChartItem('n2')];
    const sources = [s1, s2];
    expect(sourceDisplayName(s1, items, sources)).toBe('A · Graph 1');
    expect(sourceDisplayName(s2, items, sources)).toBe('B · Graph 2');
  });

  test('returns plain name with no charts', () => {
    const s1 = makeSourceWithName('n1', 'A', 'g1', 'Graph 1');
    expect(sourceDisplayName(s1, [], [s1])).toBe('A');
  });
});

// --- sourceDisplayName with forceNamespace ---

describe('sourceDisplayName with forceNamespace', () => {
  test('returns plain name when all sources are from one graph', () => {
    const s1 = makeSourceWithName('n1', 'A', 'g1', 'Graph 1');
    const s2 = makeSourceWithName('n2', 'B', 'g1', 'Graph 1');
    expect(sourceDisplayName(s1, [], [s1, s2], true)).toBe('A');
    expect(sourceDisplayName(s2, [], [s1, s2], true)).toBe('B');
  });

  test('appends graph name when sources span multiple graphs', () => {
    const s1 = makeSourceWithName('n1', 'A', 'g1', 'Graph 1');
    const s2 = makeSourceWithName('n2', 'B', 'g2', 'Graph 2');
    expect(sourceDisplayName(s1, [], [s1, s2], true)).toBe('A · Graph 1');
    expect(sourceDisplayName(s2, [], [s1, s2], true)).toBe('B · Graph 2');
  });

  test('returns plain name with single source', () => {
    const s1 = makeSourceWithName('n1', 'A', 'g1', 'Graph 1');
    expect(sourceDisplayName(s1, [], [s1], true)).toBe('A');
  });
});
