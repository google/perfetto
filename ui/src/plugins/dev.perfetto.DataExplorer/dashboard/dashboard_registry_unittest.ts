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
  DEFAULT_COL_SPAN,
  DEFAULT_ROW_SPAN,
  GRID_COLUMNS,
  checkOverlap,
  dashboardRegistry,
  findNonOverlappingPosition,
  getItemId,
  getNextItemPosition,
  getConsumersOf,
  getDriversOf,
  isDriverChart,
  parseBrushFilters,
  validateDashboardItems,
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

function makeLabelItem(id = 'label-1', text = ''): DashboardItem {
  return {kind: 'label', id, text};
}

function makeDividerItem(id = 'div-1', row = 3): DashboardItem {
  return {kind: 'divider', id, row};
}

function makeChartItem(
  sourceNodeId: string,
  chartId?: string,
  col?: number,
  row?: number,
): DashboardItem & {kind: 'chart'} {
  return {
    kind: 'chart',
    sourceNodeId,
    config: {
      id: chartId ?? testChartId(),
      column: 'id',
      chartType: 'bar',
    },
    col,
    row,
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

  test('returns id for label items', () => {
    const label = makeLabelItem('label-456');
    expect(getItemId(label)).toBe('label-456');
  });
});

describe('getNextItemPosition', () => {
  test('returns (0,0) for empty items', () => {
    const pos = getNextItemPosition([]);
    expect(pos.col).toBe(0);
    expect(pos.row).toBe(0);
  });

  test('cascades left-to-right in rows of DEFAULT_COL_SPAN', () => {
    const itemsPerRow = Math.floor(GRID_COLUMNS / DEFAULT_COL_SPAN);
    const pos1 = getNextItemPosition([makeChartItem('n1')]);
    expect(pos1.col).toBe(DEFAULT_COL_SPAN);
    expect(pos1.row).toBe(0);

    // Fill first row, next item goes to second row.
    const fullRow = Array.from({length: itemsPerRow}, () =>
      makeChartItem('n1'),
    );
    const posNextRow = getNextItemPosition(fullRow);
    expect(posNextRow.col).toBe(0);
    expect(posNextRow.row).toBe(DEFAULT_ROW_SPAN);
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

  test('validates label items', () => {
    const items = [{kind: 'label', id: 'l1', text: 'Hello'}];
    const result = validateDashboardItems(items);
    expect(result).toHaveLength(1);
    expect(result?.[0].kind).toBe('label');
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

  test('rejects label without id', () => {
    const items = [{kind: 'label', text: 'Hello'}];
    expect(validateDashboardItems(items)).toBeUndefined();
  });

  test('rejects label without text', () => {
    const items = [{kind: 'label', id: 'l1'}];
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
      {kind: 'label', id: 'l1', text: 'Ok'},
      {kind: 'label', text: 'Bad'}, // missing id
    ];
    const result = validateDashboardItems(items);
    expect(result).toHaveLength(2);
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

// --- getExportedSourcesForGraph ---

describe('getExportedSourcesForGraph', () => {
  beforeEach(() => {
    dashboardRegistry.clear();
  });

  test('returns only sources matching graphId', () => {
    dashboardRegistry.setExportedSource(makeSource('n1', 'A', [], 'g1'));
    dashboardRegistry.setExportedSource(makeSource('n2', 'B', [], 'g2'));
    dashboardRegistry.setExportedSource(makeSource('n3', 'C', [], 'g1'));
    const g1Sources = dashboardRegistry.getExportedSourcesForGraph('g1');
    expect(g1Sources).toHaveLength(2);
    expect(g1Sources.map((s) => s.name)).toEqual(['A', 'C']);
  });

  test('returns empty array for unknown graphId', () => {
    dashboardRegistry.setExportedSource(makeSource('n1', 'A', [], 'g1'));
    expect(dashboardRegistry.getExportedSourcesForGraph('g999')).toHaveLength(
      0,
    );
  });
});

// --- getItemId for dividers ---

describe('getItemId for dividers', () => {
  test('returns id for divider items', () => {
    const divider = makeDividerItem('div-123');
    expect(getItemId(divider)).toBe('div-123');
  });
});

// --- Driver/consumer relationships ---
//
// The key question for any chart is:
//   1. What drives it? (getDriversOf) — charts whose brush filters it.
//   2. What does it drive? (getConsumersOf) — charts it brush-filters.
//
// Chart X drives chart Y when there exists a divider D with X.row < D.row
// and D.row <= Y.row. isDriverChart(X) is true iff getConsumersOf(X) is
// non-empty — meaning X skips brush filters on its own SQL query.

describe('getDriversOf', () => {
  test('no dividers — nobody drives anything', () => {
    const a = makeChartItem('n1', 'a', 0, 1);
    const b = makeChartItem('n2', 'b', 0, 2);
    const items: DashboardItem[] = [a, b];
    expect(getDriversOf(a, items)).toEqual([]);
    expect(getDriversOf(b, items)).toEqual([]);
  });

  test('single divider — chart above drives chart below', () => {
    const a = makeChartItem('n1', 'a', 0, 1);
    const div1 = makeDividerItem('div-1', 3);
    const b = makeChartItem('n2', 'b', 0, 4);
    const items: DashboardItem[] = [a, div1, b];
    expect(getDriversOf(b, items)).toEqual([a]);
    expect(getDriversOf(a, items)).toEqual([]);
  });

  test('non-chart items return empty', () => {
    const label = makeLabelItem('l1', 'Hello');
    const div1 = makeDividerItem('div-1', 3);
    expect(getDriversOf(label, [label, div1])).toEqual([]);
  });
});

describe('getConsumersOf', () => {
  test('no dividers — nobody is consumed', () => {
    const a = makeChartItem('n1', 'a', 0, 1);
    const items: DashboardItem[] = [a];
    expect(getConsumersOf(a, items)).toEqual([]);
  });

  test('single divider — chart above consumes chart below', () => {
    const a = makeChartItem('n1', 'a', 0, 1);
    const div1 = makeDividerItem('div-1', 3);
    const b = makeChartItem('n2', 'b', 0, 4);
    const items: DashboardItem[] = [a, div1, b];
    expect(getConsumersOf(a, items)).toEqual([b]);
    expect(getConsumersOf(b, items)).toEqual([]);
  });

  test('chart below divider has no consumers', () => {
    const a = makeChartItem('n1', 'a', 0, 4);
    const div1 = makeDividerItem('div-1', 3);
    const items: DashboardItem[] = [a, div1];
    expect(getConsumersOf(a, items)).toEqual([]);
  });
});

describe('isDriverChart', () => {
  test('true when chart has consumers (divider below)', () => {
    const a = makeChartItem('n1', 'a', 0, 1);
    const div1 = makeDividerItem('div-1', 3);
    const b = makeChartItem('n2', 'b', 0, 4);
    const items: DashboardItem[] = [a, div1, b];
    expect(isDriverChart(a, items)).toBe(true);
  });

  test('false when chart has no consumers', () => {
    const a = makeChartItem('n1', 'a', 0, 4);
    const div1 = makeDividerItem('div-1', 3);
    const items: DashboardItem[] = [a, div1];
    expect(isDriverChart(a, items)).toBe(false);
  });
});

// --- Stacked dividers: the full picture ---
//
// Layout:  A(row=1) → div1(row=3) → B(row=4) → div2(row=6) → C(row=7)
//
// A drives B and C.  B drives C.  C drives nobody.
// Nothing drives A.  A drives B.  A and B drive C.
// Only C (no consumers) applies brush filters to its own query.

describe('stacked dividers', () => {
  function makeStack() {
    const a = makeChartItem('n1', 'a', 0, 1);
    const div1 = makeDividerItem('div-1', 3);
    const b = makeChartItem('n2', 'b', 0, 4);
    const div2 = makeDividerItem('div-2', 6);
    const c = makeChartItem('n3', 'c', 0, 7);
    const items: DashboardItem[] = [a, div1, b, div2, c];
    return {a, b, c, items};
  }

  test('A drives B and C', () => {
    const {a, b, c, items} = makeStack();
    expect(getConsumersOf(a, items)).toEqual([b, c]);
  });

  test('B drives C only', () => {
    const {b, c, items} = makeStack();
    expect(getConsumersOf(b, items)).toEqual([c]);
  });

  test('C drives nobody', () => {
    const {c, items} = makeStack();
    expect(getConsumersOf(c, items)).toEqual([]);
  });

  test('nothing drives A', () => {
    const {a, items} = makeStack();
    expect(getDriversOf(a, items)).toEqual([]);
  });

  test('A drives B', () => {
    const {a, b, items} = makeStack();
    expect(getDriversOf(b, items)).toEqual([a]);
  });

  test('A and B both drive C', () => {
    const {a, b, c, items} = makeStack();
    expect(getDriversOf(c, items)).toEqual([a, b]);
  });

  test('only C applies brush filters to itself', () => {
    const {a, b, c, items} = makeStack();
    // isDriverChart → skip own filters. Only C is not a driver.
    expect(isDriverChart(a, items)).toBe(true);
    expect(isDriverChart(b, items)).toBe(true);
    expect(isDriverChart(c, items)).toBe(false);
  });
});

// --- validateDashboardItems with dividers ---

describe('validateDashboardItems with dividers', () => {
  test('validates divider items', () => {
    const items = [{kind: 'divider', id: 'd1', row: 3}];
    const result = validateDashboardItems(items);
    expect(result).toHaveLength(1);
    expect(result?.[0].kind).toBe('divider');
  });

  test('rejects divider without id', () => {
    const items = [{kind: 'divider', row: 3}];
    expect(validateDashboardItems(items)).toBeUndefined();
  });

  test('rejects divider without row', () => {
    const items = [{kind: 'divider', id: 'd1'}];
    expect(validateDashboardItems(items)).toBeUndefined();
  });

  test('accepts mix of charts, labels, and dividers', () => {
    const items = [
      {
        kind: 'chart',
        sourceNodeId: 'n1',
        config: {id: 'c1', column: 'x', chartType: 'bar'},
      },
      {kind: 'divider', id: 'd1', row: 3},
      {kind: 'label', id: 'l1', text: 'Hello'},
    ];
    const result = validateDashboardItems(items);
    expect(result).toHaveLength(3);
  });
});

// --- checkOverlap ---

describe('checkOverlap', () => {
  test('no overlap when items are far apart', () => {
    const items: DashboardItem[] = [makeChartItem('n1', 'a', 0, 0)];
    // Place at col=16, row=0 — well away from item at (0,0) with span 8×6.
    expect(
      checkOverlap(16, 0, DEFAULT_COL_SPAN, DEFAULT_ROW_SPAN, items, 'other'),
    ).toBe(false);
  });

  test('overlap when rectangles intersect', () => {
    const items: DashboardItem[] = [makeChartItem('n1', 'a', 0, 0)];
    // Place at col=4, row=2 — overlaps the (0,0) 8×6 item.
    expect(
      checkOverlap(4, 2, DEFAULT_COL_SPAN, DEFAULT_ROW_SPAN, items, 'other'),
    ).toBe(true);
  });

  test('overlap when directly adjacent (1-cell gap enforced)', () => {
    const items: DashboardItem[] = [makeChartItem('n1', 'a', 0, 0)];
    // Place at col=8, row=0 — directly adjacent horizontally (no gap).
    expect(
      checkOverlap(8, 0, DEFAULT_COL_SPAN, DEFAULT_ROW_SPAN, items, 'other'),
    ).toBe(true);
  });

  test('no overlap with 1-cell gap', () => {
    const items: DashboardItem[] = [makeChartItem('n1', 'a', 0, 0)];
    // Place at col=9, row=0 — 1-cell gap from the 8-wide item at col=0.
    expect(
      checkOverlap(9, 0, DEFAULT_COL_SPAN, DEFAULT_ROW_SPAN, items, 'other'),
    ).toBe(false);
  });

  test('skips item with matching skipId', () => {
    const items: DashboardItem[] = [makeChartItem('n1', 'a', 0, 0)];
    // Same position as 'a' but skipId='a' — should not overlap.
    expect(
      checkOverlap(0, 0, DEFAULT_COL_SPAN, DEFAULT_ROW_SPAN, items, 'a'),
    ).toBe(false);
  });

  test('divider spans full width', () => {
    const items: DashboardItem[] = [makeDividerItem('d1', 5)];
    // Any column at row=5 should overlap the divider.
    expect(
      checkOverlap(10, 5, DEFAULT_COL_SPAN, DEFAULT_ROW_SPAN, items, 'other'),
    ).toBe(true);
  });
});

// --- findNonOverlappingPosition ---

describe('findNonOverlappingPosition', () => {
  test('returns requested position when no overlap', () => {
    const pos = findNonOverlappingPosition(
      4,
      2,
      DEFAULT_COL_SPAN,
      DEFAULT_ROW_SPAN,
      [],
      'new',
    );
    expect(pos).toEqual({col: 4, row: 2});
  });

  test('clamps negative col to 0', () => {
    const pos = findNonOverlappingPosition(
      -5,
      0,
      DEFAULT_COL_SPAN,
      DEFAULT_ROW_SPAN,
      [],
      'new',
    );
    expect(pos.col).toBe(0);
  });

  test('clamps col so item fits within grid', () => {
    const pos = findNonOverlappingPosition(
      GRID_COLUMNS,
      0,
      DEFAULT_COL_SPAN,
      DEFAULT_ROW_SPAN,
      [],
      'new',
    );
    expect(pos.col).toBe(GRID_COLUMNS - DEFAULT_COL_SPAN);
  });

  test('finds next free position when requested is occupied', () => {
    const items: DashboardItem[] = [makeChartItem('n1', 'a', 0, 0)];
    const pos = findNonOverlappingPosition(
      0,
      0,
      DEFAULT_COL_SPAN,
      DEFAULT_ROW_SPAN,
      items,
      'new',
    );
    // Should not overlap with 'a' at (0,0).
    expect(
      checkOverlap(
        pos.col,
        pos.row,
        DEFAULT_COL_SPAN,
        DEFAULT_ROW_SPAN,
        items,
        'new',
      ),
    ).toBe(false);
  });

  test('scans to next row when current row is full', () => {
    // Fill the first row with items spaced by DEFAULT_COL_SPAN + 1 gap.
    const items: DashboardItem[] = [];
    for (
      let col = 0;
      col + DEFAULT_COL_SPAN <= GRID_COLUMNS;
      col += DEFAULT_COL_SPAN + 1
    ) {
      items.push(makeChartItem('n1', `c${col}`, col, 0));
    }
    const pos = findNonOverlappingPosition(
      0,
      0,
      DEFAULT_COL_SPAN,
      DEFAULT_ROW_SPAN,
      items,
      'new',
    );
    // Should land on a later row since row 0 is packed.
    expect(pos.row).toBeGreaterThan(0);
  });
});
