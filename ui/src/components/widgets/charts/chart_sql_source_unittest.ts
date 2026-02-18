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

import {buildChartQuery, ColumnSchema, QueryConfig} from './chart_sql_source';

const QUERY = 'SELECT name, dur, ts, cpu, size, category FROM slice';
const SCHEMA: ColumnSchema = {
  name: 'text',
  dur: 'real',
  ts: 'real',
  cpu: 'text',
  size: 'real',
  category: 'text',
};

function normalizeWhitespace(sql: string): string {
  return sql.replace(/\s+/g, ' ').trim();
}

function build(config: QueryConfig): string {
  return buildChartQuery(QUERY, SCHEMA, config);
}

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------

test('validates column names in schema', () => {
  expect(() =>
    buildChartQuery(
      'SELECT * FROM t',
      {'bad column': 'text'},
      {
        type: 'aggregated',
        dimensions: [{column: 'bad column'}],
        measures: [{column: 'bad column', aggregation: 'SUM'}],
      },
    ),
  ).toThrow('Invalid SQL column name');
});

test('buildChartQuery throws for unknown column', () => {
  expect(() =>
    build({
      type: 'aggregated',
      dimensions: [{column: 'nonexistent'}],
      measures: [{column: 'dur', aggregation: 'SUM'}],
    }),
  ).toThrow("Column 'nonexistent' not found in schema");
});

// ---------------------------------------------------------------------------
// Aggregated: simple
// ---------------------------------------------------------------------------

test('aggregated: simple bar chart query', () => {
  const sql = build({
    type: 'aggregated',
    dimensions: [{column: 'name'}],
    measures: [{column: 'dur', aggregation: 'SUM'}],
  });
  const norm = normalizeWhitespace(sql);
  expect(norm).toContain('CAST(name AS TEXT) AS _dim');
  expect(norm).toContain('SUM(dur) AS _value');
  expect(norm).toContain('GROUP BY name');
  expect(norm).toContain('ORDER BY _value DESC');
});

test('aggregated: with limit', () => {
  const sql = build({
    type: 'aggregated',
    dimensions: [{column: 'name'}],
    measures: [{column: 'dur', aggregation: 'SUM'}],
    limit: 10,
  });
  expect(normalizeWhitespace(sql)).toContain('LIMIT 10');
});

test('aggregated: ascending order', () => {
  const sql = build({
    type: 'aggregated',
    dimensions: [{column: 'name'}],
    measures: [{column: 'dur', aggregation: 'AVG'}],
    orderDirection: 'asc',
  });
  const norm = normalizeWhitespace(sql);
  expect(norm).toContain('AVG(dur) AS _value');
  expect(norm).toContain('ORDER BY _value ASC');
});

test('aggregated: with IN filter', () => {
  const sql = build({
    type: 'aggregated',
    dimensions: [{column: 'name'}],
    measures: [{column: 'dur', aggregation: 'SUM'}],
    filters: [{field: 'name', op: 'in', value: ['foo', 'bar']}],
  });
  const norm = normalizeWhitespace(sql);
  expect(norm).toContain("WHERE (name IN ('foo', 'bar'))");
});

test('aggregated: with range filter', () => {
  const sql = build({
    type: 'aggregated',
    dimensions: [{column: 'name'}],
    measures: [{column: 'dur', aggregation: 'SUM'}],
    filters: [
      {field: 'dur', op: '>=', value: 100},
      {field: 'dur', op: '<=', value: 500},
    ],
  });
  const norm = normalizeWhitespace(sql);
  expect(norm).toContain('WHERE (dur >= 100) AND (dur <= 500)');
});

test('aggregated: custom aliases', () => {
  const sql = build({
    type: 'aggregated',
    dimensions: [{column: 'name', alias: '_label'}],
    measures: [{column: 'dur', aggregation: 'SUM', alias: '_size'}],
  });
  const norm = normalizeWhitespace(sql);
  expect(norm).toContain('CAST(name AS TEXT) AS _label');
  expect(norm).toContain('SUM(dur) AS _size');
  expect(norm).toContain('ORDER BY _size DESC');
});

test('aggregated: multiple dimensions default aliases', () => {
  const sql = build({
    type: 'aggregated',
    dimensions: [{column: 'category'}, {column: 'name'}],
    measures: [{column: 'dur', aggregation: 'SUM'}],
  });
  const norm = normalizeWhitespace(sql);
  expect(norm).toContain('CAST(category AS TEXT) AS _dim');
  expect(norm).toContain('CAST(name AS TEXT) AS _dim_1');
  expect(norm).toContain('GROUP BY category, name');
});

test('aggregated: COUNT_DISTINCT aggregation', () => {
  const sql = build({
    type: 'aggregated',
    dimensions: [{column: 'name'}],
    measures: [{column: 'cpu', aggregation: 'COUNT_DISTINCT'}],
  });
  expect(normalizeWhitespace(sql)).toContain('COUNT(DISTINCT cpu) AS _value');
});

test('aggregated: multiple measures default aliases', () => {
  const sql = build({
    type: 'aggregated',
    dimensions: [{column: 'name'}],
    measures: [
      {column: 'dur', aggregation: 'SUM'},
      {column: 'size', aggregation: 'AVG'},
    ],
  });
  const norm = normalizeWhitespace(sql);
  expect(norm).toContain('SUM(dur) AS _value');
  expect(norm).toContain('AVG(size) AS _value_1');
  expect(norm).toContain('ORDER BY _value DESC');
});

// ---------------------------------------------------------------------------
// Aggregated: top-N with "(Other)"
// ---------------------------------------------------------------------------

test('aggregated: top-N with Other bucket', () => {
  const sql = build({
    type: 'aggregated',
    dimensions: [{column: 'name'}],
    measures: [{column: 'dur', aggregation: 'SUM'}],
    limit: 5,
    includeOther: true,
  });
  const norm = normalizeWhitespace(sql);
  expect(norm).toContain('WITH _agg AS');
  expect(norm).toContain('_top AS');
  expect(norm).toContain('LIMIT 5');
  expect(norm).toContain("'(Other)' AS _dim");
  expect(norm).toContain('UNION ALL');
  expect(norm).toContain('WHERE _value > 0');
});

test('aggregated: includeOther without limit is ignored', () => {
  const sql = build({
    type: 'aggregated',
    dimensions: [{column: 'name'}],
    measures: [{column: 'dur', aggregation: 'SUM'}],
    includeOther: true,
    // no limit
  });
  const norm = normalizeWhitespace(sql);
  // Should fall back to simple aggregated (no CTE)
  expect(norm).not.toContain('WITH _agg AS');
  expect(norm).not.toContain('(Other)');
});

// ---------------------------------------------------------------------------
// Aggregated: hierarchical (per-group limit)
// ---------------------------------------------------------------------------

test('aggregated: hierarchical with limitPerGroup', () => {
  const sql = build({
    type: 'aggregated',
    dimensions: [{column: 'category'}, {column: 'name'}],
    measures: [{column: 'size', aggregation: 'SUM'}],
    limitPerGroup: 10,
  });
  const norm = normalizeWhitespace(sql);
  expect(norm).toContain('WITH _agg AS');
  expect(norm).toContain('_ranked AS');
  expect(norm).toContain('ROW_NUMBER() OVER (PARTITION BY _dim');
  expect(norm).toContain('WHERE _rank <= 10');
  expect(norm).toContain('ORDER BY _dim, _value DESC');
});

test('aggregated: hierarchical with custom aliases', () => {
  const sql = build({
    type: 'aggregated',
    dimensions: [
      {column: 'category', alias: '_group'},
      {column: 'name', alias: '_label'},
    ],
    measures: [{column: 'size', aggregation: 'SUM'}],
    limitPerGroup: 5,
  });
  const norm = normalizeWhitespace(sql);
  expect(norm).toContain('CAST(category AS TEXT) AS _group');
  expect(norm).toContain('CAST(name AS TEXT) AS _label');
  expect(norm).toContain('PARTITION BY _group');
  expect(norm).toContain('ORDER BY _group, _value DESC');
});

test('aggregated: hierarchical with filter', () => {
  const sql = build({
    type: 'aggregated',
    dimensions: [{column: 'category'}, {column: 'name'}],
    measures: [{column: 'size', aggregation: 'SUM'}],
    limitPerGroup: 10,
    filters: [{field: 'category', op: 'in', value: ['A', 'B']}],
  });
  const norm = normalizeWhitespace(sql);
  expect(norm).toContain("WHERE (category IN ('A', 'B'))");
});

// ---------------------------------------------------------------------------
// Points queries
// ---------------------------------------------------------------------------

test('points: basic line chart query', () => {
  const sql = build({
    type: 'points',
    columns: [
      {column: 'ts', alias: '_x', cast: 'real'},
      {column: 'dur', alias: '_y', cast: 'real'},
    ],
    orderBy: [{column: '_x'}],
  });
  const norm = normalizeWhitespace(sql);
  expect(norm).toContain('CAST(ts AS REAL) AS _x');
  expect(norm).toContain('CAST(dur AS REAL) AS _y');
  expect(norm).toContain('ORDER BY _x ASC');
});

test('points: with breakdown (series)', () => {
  const sql = build({
    type: 'points',
    columns: [
      {column: 'ts', alias: '_x', cast: 'real'},
      {column: 'dur', alias: '_y', cast: 'real'},
    ],
    breakdown: 'cpu',
    orderBy: [{column: '_series'}, {column: '_x'}],
  });
  const norm = normalizeWhitespace(sql);
  expect(norm).toContain('CAST(cpu AS TEXT) AS _series');
  expect(norm).toContain('ORDER BY _series ASC, _x ASC');
});

test('points: with range filters', () => {
  const sql = build({
    type: 'points',
    columns: [
      {column: 'ts', alias: '_x', cast: 'real'},
      {column: 'dur', alias: '_y', cast: 'real'},
    ],
    filters: [
      {field: 'ts', op: '>=', value: 1000},
      {field: 'ts', op: '<=', value: 2000},
    ],
  });
  const norm = normalizeWhitespace(sql);
  expect(norm).toContain('WHERE (ts >= 1000) AND (ts <= 2000)');
});

test('points: scatter with size and label', () => {
  const sql = build({
    type: 'points',
    columns: [
      {column: 'ts', alias: '_x', cast: 'real'},
      {column: 'dur', alias: '_y', cast: 'real'},
      {column: 'size', alias: '_size', cast: 'real'},
      {column: 'name', alias: '_label', cast: 'text'},
    ],
    breakdown: 'category',
    orderBy: [{column: '_series'}],
  });
  const norm = normalizeWhitespace(sql);
  expect(norm).toContain('CAST(size AS REAL) AS _size');
  expect(norm).toContain('CAST(name AS TEXT) AS _label');
  expect(norm).toContain('CAST(category AS TEXT) AS _series');
});

test('points: null column (column omitted)', () => {
  const sql = build({
    type: 'points',
    columns: [
      {column: 'ts', alias: '_x', cast: 'real'},
      {alias: '_size', cast: 'real'},
    ],
  });
  const norm = normalizeWhitespace(sql);
  expect(norm).toContain('CAST(ts AS REAL) AS _x');
  expect(norm).toContain('NULL AS _size');
});

test('points: no order by', () => {
  const sql = build({
    type: 'points',
    columns: [{column: 'ts', alias: '_x', cast: 'real'}],
  });
  expect(normalizeWhitespace(sql)).not.toContain('ORDER BY');
});

test('points: throws for unknown column', () => {
  expect(() =>
    build({
      type: 'points',
      columns: [{column: 'nope', alias: '_x', cast: 'real'}],
    }),
  ).toThrow("Column 'nope' not found in schema");
});

test('points: throws for unknown breakdown column', () => {
  expect(() =>
    build({
      type: 'points',
      columns: [{column: 'ts', alias: '_x', cast: 'real'}],
      breakdown: 'nope',
    }),
  ).toThrow("Column 'nope' not found in schema");
});

// ---------------------------------------------------------------------------
// Points: stride-sampled (maxPointsPerSeries)
// ---------------------------------------------------------------------------

test('points: maxPointsPerSeries with breakdown', () => {
  const sql = build({
    type: 'points',
    columns: [
      {column: 'ts', alias: '_x', cast: 'real'},
      {column: 'dur', alias: '_y', cast: 'real'},
    ],
    breakdown: 'cpu',
    maxPointsPerSeries: 500,
    orderBy: [{column: '_series'}],
  });
  const norm = normalizeWhitespace(sql);
  expect(norm).toContain('ROW_NUMBER() OVER (PARTITION BY CAST(cpu AS TEXT))');
  expect(norm).toContain('COUNT(*) OVER (PARTITION BY CAST(cpu AS TEXT))');
  expect(norm).toContain('(_cnt + 500 - 1) / 500');
  expect(norm).toContain('WHERE (_rn - 1) %');
  expect(norm).toContain('SELECT _x, _y, _series FROM');
});

test('points: maxPointsPerSeries without breakdown', () => {
  const sql = build({
    type: 'points',
    columns: [
      {column: 'ts', alias: '_x', cast: 'real'},
      {column: 'dur', alias: '_y', cast: 'real'},
    ],
    maxPointsPerSeries: 1000,
  });
  const norm = normalizeWhitespace(sql);
  expect(norm).toContain('PARTITION BY 1');
  expect(norm).toContain('(_cnt + 1000 - 1) / 1000');
  expect(norm).toContain('SELECT _x, _y FROM');
});

test('points: maxPointsPerSeries with filters', () => {
  const sql = build({
    type: 'points',
    columns: [
      {column: 'ts', alias: '_x', cast: 'real'},
      {column: 'dur', alias: '_y', cast: 'real'},
    ],
    breakdown: 'cpu',
    maxPointsPerSeries: 200,
    filters: [
      {field: 'ts', op: '>=', value: 100},
      {field: 'ts', op: '<=', value: 500},
    ],
  });
  const norm = normalizeWhitespace(sql);
  expect(norm).toContain('WHERE (ts >= 100) AND (ts <= 500)');
  expect(norm).toContain('WHERE (_rn - 1) %');
});

test('points: without maxPointsPerSeries produces no window functions', () => {
  const sql = build({
    type: 'points',
    columns: [
      {column: 'ts', alias: '_x', cast: 'real'},
      {column: 'dur', alias: '_y', cast: 'real'},
    ],
  });
  const norm = normalizeWhitespace(sql);
  expect(norm).not.toContain('ROW_NUMBER');
  expect(norm).not.toContain('_rn');
  expect(norm).not.toContain('_cnt');
});

// ---------------------------------------------------------------------------
// Histogram queries
// ---------------------------------------------------------------------------

test('histogram: basic query', () => {
  const sql = build({
    type: 'histogram',
    valueColumn: 'dur',
    bucketCount: 20,
  });
  const norm = normalizeWhitespace(sql);
  expect(norm).toContain('WITH _data AS');
  expect(norm).toContain('dur AS _value');
  expect(norm).toContain('(SELECT MIN(_value) FROM _data) AS _min');
  expect(norm).toContain('(SELECT MAX(_value) FROM _data) AS _max');
  expect(norm).toContain('(SELECT COUNT(*) FROM _data) AS _total');
  expect(norm).toContain('AS _bucket_idx');
  expect(norm).toContain('COUNT(*) AS _count');
  expect(norm).toContain('GROUP BY _bucket_idx');
  expect(norm).toContain('ORDER BY _bucket_idx');
  // bucket count of 20 means max index is 19
  expect(norm).toContain('THEN 19');
  expect(norm).toContain('MIN(19,');
  expect(norm).toContain('/ 20.0)');
});

test('histogram: with range filter', () => {
  const sql = build({
    type: 'histogram',
    valueColumn: 'dur',
    bucketCount: 10,
    filters: [
      {field: 'dur', op: '>=', value: 100},
      {field: 'dur', op: '<=', value: 5000},
    ],
  });
  const norm = normalizeWhitespace(sql);
  expect(norm).toContain('WHERE (dur >= 100) AND (dur <= 5000)');
});

test('histogram: throws for unknown column', () => {
  expect(() =>
    build({
      type: 'histogram',
      valueColumn: 'nope',
      bucketCount: 10,
    }),
  ).toThrow("Column 'nope' not found in schema");
});

// ---------------------------------------------------------------------------
// Edge cases
// ---------------------------------------------------------------------------

test('aggregated: no filters produces no WHERE clause', () => {
  const sql = build({
    type: 'aggregated',
    dimensions: [{column: 'name'}],
    measures: [{column: 'dur', aggregation: 'SUM'}],
  });
  expect(normalizeWhitespace(sql)).not.toContain('WHERE');
});

test('aggregated: empty filters array produces no WHERE clause', () => {
  const sql = build({
    type: 'aggregated',
    dimensions: [{column: 'name'}],
    measures: [{column: 'dur', aggregation: 'SUM'}],
    filters: [],
  });
  expect(normalizeWhitespace(sql)).not.toContain('WHERE');
});
