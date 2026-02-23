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

import {assertUnreachable} from '../../../base/assert';
import {
  QuerySlot,
  SerialTaskQueue,
  type QueryResult,
} from '../../../base/query_slot';
import type {Engine} from '../../../trace_processor/engine';
import type {QueryResult as TPQueryResult} from '../../../trace_processor/query_result';
import {AggregateFunction, Filter} from '../datagrid/model';
import {filterToSql, sqlAggregateExpr} from '../datagrid/sql_utils';
import {validateColumnName} from './chart_utils';

// ---------------------------------------------------------------------------
// Schema types
// ---------------------------------------------------------------------------

/**
 * Column type in the source query.
 * 'text' columns are cast to TEXT (dimensions, breakdowns).
 * 'real' columns are cast to REAL (measures, x/y axes).
 */
export type ColumnType = 'text' | 'real';

/**
 * Schema definition mapping column names to their types.
 * Currently used only for column existence validation; the actual cast
 * direction is determined by the query config (dimensions → TEXT,
 * measures → aggregation, point columns → explicit cast spec).
 */
export type ColumnSchema = Readonly<Record<string, ColumnType>>;

// ---------------------------------------------------------------------------
// Query configuration types
// ---------------------------------------------------------------------------

/**
 * Dimension: a column used for grouping (GROUP BY), cast to TEXT.
 */
export interface DimensionSpec {
  readonly column: string;
  /** Output alias. Defaults to `_dim` for the first, `_dim_1`, `_dim_2`... */
  readonly alias?: string;
}

/**
 * Measure: a column aggregated numerically.
 */
export interface MeasureSpec {
  readonly column: string;
  readonly aggregation: AggregateFunction;
  /** Output alias. Defaults to `_value` for the first, `_value_1`, `_value_2`... */
  readonly alias?: string;
}

/**
 * Point column: a raw column selected in a points query.
 * When `column` is omitted, produces `NULL AS alias` in the SQL.
 */
export interface PointColumnSpec {
  readonly column?: string;
  readonly alias: string;
  readonly cast: 'real' | 'text';
}

// ---------------------------------------------------------------------------
// Three query mode configs (discriminated union)
// ---------------------------------------------------------------------------

/**
 * Aggregated query (bar, pie, treemap).
 *
 * Produces: `SELECT dims, AGG(measures) FROM source [WHERE] GROUP BY dims`
 */
export interface AggregatedQueryConfig {
  readonly type: 'aggregated';

  /** Grouping dimensions (at least 1). */
  readonly dimensions: ReadonlyArray<DimensionSpec>;

  /** Measures to compute (at least 1). */
  readonly measures: ReadonlyArray<MeasureSpec>;

  /** Filters applied before aggregation. */
  readonly filters?: ReadonlyArray<Filter>;

  /** Sort direction for the first measure. Defaults to 'desc'. */
  readonly orderDirection?: 'asc' | 'desc';

  /** Limit number of output rows. */
  readonly limit?: number;

  /**
   * When true and `limit` is set, remaining rows beyond the top-N are
   * collapsed into a single "(Other)" row with summed measure values.
   */
  readonly includeOther?: boolean;

  /**
   * Limit results per group (first dimension). Uses ROW_NUMBER() OVER
   * (PARTITION BY first_dim ORDER BY value DESC). Only valid when
   * dimensions.length >= 2.
   */
  readonly limitPerGroup?: number;
}

/**
 * Points/raw query (line, scatter).
 *
 * Produces: `SELECT columns [, breakdown] FROM source [WHERE] [ORDER BY]`
 */
export interface PointsQueryConfig {
  readonly type: 'points';

  /** Columns to select. */
  readonly columns: ReadonlyArray<PointColumnSpec>;

  /** Optional breakdown/series column (cast to TEXT, aliased as `_series`). */
  readonly breakdown?: string;

  /** Filters to apply. */
  readonly filters?: ReadonlyArray<Filter>;

  /** Sort specification. */
  readonly orderBy?: ReadonlyArray<{
    readonly column: string;
    readonly direction?: 'asc' | 'desc';
  }>;

  /**
   * When set, stride-samples each series (PARTITION BY _series) down to at
   * most this many rows in SQL using ROW_NUMBER window functions. This avoids
   * fetching millions of rows into JS when only a representative subset is
   * needed (e.g., scatter charts).
   */
  readonly maxPointsPerSeries?: number;
}

/**
 * Histogram query.
 *
 * Produces a CTE that buckets values and returns counts per bucket along
 * with min/max/total metadata.
 */
export interface HistogramQueryConfig {
  readonly type: 'histogram';

  /** Column to bucket. */
  readonly valueColumn: string;

  /** Number of buckets. */
  readonly bucketCount: number;

  /** Filters to apply before bucketing. */
  readonly filters?: ReadonlyArray<Filter>;
}

/** Union of all query config types. */
export type QueryConfig =
  | AggregatedQueryConfig
  | PointsQueryConfig
  | HistogramQueryConfig;

// ---------------------------------------------------------------------------
// buildChartQuery — standalone SQL query builder
// ---------------------------------------------------------------------------

/**
 * Build a SQL query string from the given configuration.
 * Validates schema column names and that all referenced columns exist.
 *
 * @param query Base SQL query (used as a subquery).
 * @param schema Column schema mapping column names to types.
 * @param config Query configuration (aggregated, points, or histogram).
 */
export function buildChartQuery(
  query: string,
  schema: ColumnSchema,
  config: QueryConfig,
): string {
  for (const col of Object.keys(schema)) {
    validateColumnName(col);
  }
  switch (config.type) {
    case 'aggregated':
      return buildAggregated(query, schema, config);
    case 'points':
      return buildPoints(query, schema, config);
    case 'histogram':
      return buildHistogram(query, schema, config);
    default:
      assertUnreachable(config);
  }
}

// ---------------------------------------------------------------------------
// Aggregated queries
// ---------------------------------------------------------------------------

function buildAggregated(
  query: string,
  schema: ColumnSchema,
  config: AggregatedQueryConfig,
): string {
  for (const dim of config.dimensions) {
    assertColumn(dim.column, schema);
  }
  for (const meas of config.measures) {
    assertColumn(meas.column, schema);
  }

  // Decide which variant to use
  if (config.includeOther && config.limit !== undefined) {
    return buildTopNWithOther(query, config);
  }
  if (config.limitPerGroup !== undefined && config.dimensions.length >= 2) {
    return buildHierarchical(query, config);
  }
  return buildSimpleAggregated(query, config);
}

function buildSimpleAggregated(
  query: string,
  config: AggregatedQueryConfig,
): string {
  const selectParts = [
    ...dimSelectExprs(config.dimensions),
    ...measureSelectExprs(config.measures),
  ];

  const whereClause = buildWhereClause(config.filters);
  const groupByExprs = config.dimensions.map((d) => d.column);
  const direction = config.orderDirection ?? 'desc';
  const orderAlias = measureAlias(config.measures, 0);
  const limitClause = config.limit !== undefined ? `LIMIT ${config.limit}` : '';

  return `
SELECT
  ${selectParts.join(',\n  ')}
FROM (${query})
${whereClause}
GROUP BY ${groupByExprs.join(', ')}
ORDER BY ${orderAlias} ${direction.toUpperCase()}
${limitClause}`.trim();
}

function buildTopNWithOther(
  query: string,
  config: AggregatedQueryConfig,
): string {
  const selectParts = [
    ...dimSelectExprs(config.dimensions),
    ...measureSelectExprs(config.measures),
  ];
  const whereClause = buildWhereClause(config.filters);
  const groupByExprs = config.dimensions.map((d) => d.column);
  const direction = config.orderDirection ?? 'desc';
  const orderCol = measureAlias(config.measures, 0);
  // Caller guarantees config.limit is defined (checked in buildAggregated).
  const limit = config.limit ?? 0;

  // Collect output column aliases
  const dimAliases = config.dimensions.map((d, i) => dimAlias(d, i));
  const measAliases = config.measures.map((_, i) =>
    measureAlias(config.measures, i),
  );
  const allAliases = [...dimAliases, ...measAliases];
  const aliasList = allAliases.join(', ');

  // First dim alias for the "Other" label
  const firstDimAlias = dimAliases[0];

  // Build "(Other)" select: first dim = '(Other)', rest of dims = NULL,
  // each measure = SUM(measure_alias)
  const otherDimExprs = dimAliases.map((alias, i) =>
    i === 0 ? `'(Other)' AS ${alias}` : `NULL AS ${alias}`,
  );
  const otherMeasExprs = measAliases.map(
    (alias) => `SUM(${alias}) AS ${alias}`,
  );
  const otherSelectParts = [...otherDimExprs, ...otherMeasExprs];

  return `
WITH _agg AS (
  SELECT
    ${selectParts.join(',\n    ')}
  FROM (${query})
  ${whereClause}
  GROUP BY ${groupByExprs.join(', ')}
  ORDER BY ${orderCol} ${direction.toUpperCase()}
),
_top AS (
  SELECT ${aliasList} FROM _agg ORDER BY ${orderCol} ${direction.toUpperCase()} LIMIT ${limit}
),
_other AS (
  SELECT
    ${otherSelectParts.join(',\n    ')}
  FROM _agg
  WHERE ${firstDimAlias} NOT IN (SELECT ${firstDimAlias} FROM _top)
)
SELECT ${aliasList} FROM _top
UNION ALL
SELECT ${aliasList} FROM _other WHERE ${measAliases[0]} > 0`.trim();
}

function buildHierarchical(
  query: string,
  config: AggregatedQueryConfig,
): string {
  const selectParts = [
    ...dimSelectExprs(config.dimensions),
    ...measureSelectExprs(config.measures),
  ];
  const whereClause = buildWhereClause(config.filters);
  const groupByExprs = config.dimensions.map((d) => d.column);
  const direction = config.orderDirection ?? 'desc';
  const firstDimCol = dimAlias(config.dimensions[0], 0);
  const orderCol = measureAlias(config.measures, 0);

  const dimAliases = config.dimensions.map((d, i) => dimAlias(d, i));
  const measAliases = config.measures.map((_, i) =>
    measureAlias(config.measures, i),
  );
  const allAliases = [...dimAliases, ...measAliases];
  const aliasList = allAliases.join(', ');
  // Caller guarantees config.limitPerGroup is defined (checked in
  // buildAggregated).
  const limitPerGroup = config.limitPerGroup ?? 0;

  return `
WITH _agg AS (
  SELECT
    ${selectParts.join(',\n    ')}
  FROM (${query})
  ${whereClause}
  GROUP BY ${groupByExprs.join(', ')}
),
_ranked AS (
  SELECT
    ${aliasList},
    ROW_NUMBER() OVER (PARTITION BY ${firstDimCol} ORDER BY ${orderCol} ${direction.toUpperCase()}) AS _rank
  FROM _agg
)
SELECT ${aliasList}
FROM _ranked
WHERE _rank <= ${limitPerGroup}
ORDER BY ${firstDimCol}, ${orderCol} ${direction.toUpperCase()}`.trim();
}

// ---------------------------------------------------------------------------
// Points queries
// ---------------------------------------------------------------------------

function buildPoints(
  query: string,
  schema: ColumnSchema,
  config: PointsQueryConfig,
): string {
  for (const col of config.columns) {
    if (col.column !== undefined) {
      assertColumn(col.column, schema);
    }
    validateColumnName(col.alias);
  }
  if (config.breakdown !== undefined) {
    assertColumn(config.breakdown, schema);
  }

  const selectParts = config.columns.map((col) =>
    col.column !== undefined
      ? `CAST(${col.column} AS ${sqlCastType(col.cast)}) AS ${col.alias}`
      : `NULL AS ${col.alias}`,
  );

  if (config.breakdown !== undefined) {
    selectParts.push(`CAST(${config.breakdown} AS TEXT) AS _series`);
  }

  const whereClause = buildWhereClause(config.filters);

  let orderByClause = '';
  if (config.orderBy !== undefined && config.orderBy.length > 0) {
    const orderParts = config.orderBy.map(
      (o) => `${o.column} ${(o.direction ?? 'asc').toUpperCase()}`,
    );
    orderByClause = `ORDER BY ${orderParts.join(', ')}`;
  }

  if (config.maxPointsPerSeries !== undefined) {
    return buildStrideSampledPoints(
      query,
      selectParts,
      whereClause,
      orderByClause,
      config,
    );
  }

  return `
SELECT
  ${selectParts.join(',\n  ')}
FROM (${query})
${whereClause}
${orderByClause}`.trim();
}

/**
 * Wraps the base points query in a subquery that uses ROW_NUMBER() and
 * COUNT() window functions to stride-sample each series down to at most
 * `maxPointsPerSeries` rows. This keeps large datasets from being fully
 * transferred into JS memory.
 */
function buildStrideSampledPoints(
  query: string,
  selectParts: string[],
  whereClause: string,
  orderByClause: string,
  config: PointsQueryConfig,
): string {
  const maxPts = config.maxPointsPerSeries ?? 0;
  const aliases = config.columns.map((col) => col.alias);
  if (config.breakdown !== undefined) {
    aliases.push('_series');
  }
  const aliasList = aliases.join(', ');

  // Use the full expression (not the alias) because SQLite cannot
  // resolve column aliases inside OVER clauses of the same SELECT.
  const partitionExpr =
    config.breakdown !== undefined
      ? `PARTITION BY CAST(${config.breakdown} AS TEXT)`
      : 'PARTITION BY 1';

  return `
SELECT ${aliasList}
FROM (
  SELECT
    ${selectParts.join(',\n    ')},
    ROW_NUMBER() OVER (${partitionExpr}) AS _rn,
    COUNT(*) OVER (${partitionExpr}) AS _cnt
  FROM (${query})
  ${whereClause}
)
WHERE (_rn - 1) % MAX(1, (_cnt + ${maxPts} - 1) / ${maxPts}) = 0
${orderByClause}`.trim();
}

// ---------------------------------------------------------------------------
// Histogram queries
// ---------------------------------------------------------------------------

function buildHistogram(
  query: string,
  schema: ColumnSchema,
  config: HistogramQueryConfig,
): string {
  assertColumn(config.valueColumn, schema);

  const col = config.valueColumn;
  const bucketCount = config.bucketCount;
  const whereClause = buildWhereClause(config.filters);

  return `
WITH _data AS (
  SELECT ${col} AS _value
  FROM (${query})
  ${whereClause}
)
SELECT
  (SELECT MIN(_value) FROM _data) AS _min,
  (SELECT MAX(_value) FROM _data) AS _max,
  (SELECT COUNT(*) FROM _data) AS _total,
  CASE
    WHEN (SELECT MAX(_value) FROM _data) = (SELECT MIN(_value) FROM _data) THEN 0
    WHEN _value = (SELECT MAX(_value) FROM _data) THEN ${bucketCount - 1}
    ELSE MIN(${bucketCount - 1}, CAST(
      (_value - (SELECT MIN(_value) FROM _data)) /
      (((SELECT MAX(_value) FROM _data) - (SELECT MIN(_value) FROM _data)) / ${bucketCount}.0)
    AS INT))
  END AS _bucket_idx,
  COUNT(*) AS _count
FROM _data
GROUP BY _bucket_idx
ORDER BY _bucket_idx`.trim();
}

// ---------------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------------

function assertColumn(name: string, schema: ColumnSchema): void {
  if (!(name in schema)) {
    throw new Error(
      `Column '${name}' not found in schema. ` +
        `Available: ${Object.keys(schema).join(', ')}`,
    );
  }
}

function buildWhereClause(filters: ReadonlyArray<Filter> | undefined): string {
  if (filters === undefined || filters.length === 0) return '';
  const conditions = filters.map((f) => `(${filterToSql(f, f.field)})`);
  return `WHERE ${conditions.join(' AND ')}`;
}

function dimAlias(dim: DimensionSpec, index: number): string {
  if (dim.alias !== undefined) {
    validateColumnName(dim.alias);
    return dim.alias;
  }
  return index === 0 ? '_dim' : `_dim_${index}`;
}

function measureAlias(
  measures: ReadonlyArray<MeasureSpec>,
  index: number,
): string {
  const meas = measures[index];
  if (meas.alias !== undefined) {
    validateColumnName(meas.alias);
    return meas.alias;
  }
  return index === 0 ? '_value' : `_value_${index}`;
}

function dimSelectExprs(dims: ReadonlyArray<DimensionSpec>): string[] {
  return dims.map(
    (dim, i) => `CAST(${dim.column} AS TEXT) AS ${dimAlias(dim, i)}`,
  );
}

function measureSelectExprs(measures: ReadonlyArray<MeasureSpec>): string[] {
  return measures.map(
    (meas, i) =>
      `${sqlAggregateExpr(meas.aggregation, meas.column)} AS ${measureAlias(measures, i)}`,
  );
}

function sqlCastType(cast: 'real' | 'text'): string {
  return cast === 'real' ? 'REAL' : 'TEXT';
}

// ---------------------------------------------------------------------------
// createChartLoader — composition-based chart loader factory
// ---------------------------------------------------------------------------

/**
 * A chart loader with `use()` / `dispose()` lifecycle.
 * Call `use()` every render cycle; call `dispose()` in `onremove`.
 */
export interface ChartLoader<TConfig, TData> {
  use(config: TConfig): QueryResult<TData>;
  dispose(): void;
}

/**
 * Options for creating a chart loader.
 */
export interface ChartLoaderOpts<TConfig, TData> {
  /** The trace processor engine to run queries against. */
  readonly engine: Engine;

  /** Base SQL query (used as a subquery). */
  readonly query: string;

  /** Column schema mapping column names to types. */
  readonly schema: ColumnSchema;

  /** Build the QueryConfig from the per-use config. */
  readonly buildQueryConfig: (config: TConfig) => QueryConfig;

  /** Parse query result rows into chart-specific data. */
  readonly parseResult: (queryResult: TPQueryResult, config: TConfig) => TData;

  /**
   * Extra fields to add to the cache key for post-processing params
   * that don't affect the SQL but do affect the output.
   */
  readonly extraCacheKey?: (
    config: TConfig,
  ) => Record<string, string | number | boolean | undefined>;
}

/**
 * Create a SQL-backed chart loader using composition.
 *
 * Handles QuerySlot lifecycle, query execution via Engine, and caching.
 * The caller provides `buildQueryConfig` and `parseResult` callbacks
 * to customise SQL generation and result parsing.
 */
export function createChartLoader<TConfig, TData>(
  opts: ChartLoaderOpts<TConfig, TData>,
): ChartLoader<TConfig, TData> {
  // Validate schema column names once at creation time.
  for (const col of Object.keys(opts.schema)) {
    validateColumnName(col);
  }

  const taskQueue = new SerialTaskQueue();
  const querySlot = new QuerySlot<TData>(taskQueue);

  return {
    use(config: TConfig): QueryResult<TData> {
      const queryConfig = opts.buildQueryConfig(config);
      const sql = buildChartQuery(opts.query, opts.schema, queryConfig);
      const extra = opts.extraCacheKey?.(config) ?? {};
      const key = {sql, ...extra};
      return querySlot.use({
        key,
        queryFn: async () => {
          const queryResult = await opts.engine.query(sql);
          return opts.parseResult(queryResult, config);
        },
        // Retain stale chart data while new queries are in flight (e.g., during
        // brush/filter changes) to avoid flashing a loading spinner.
        retainOn: Object.keys(key) as (keyof typeof key)[],
      });
    },
    dispose(): void {
      querySlot.dispose();
    },
  };
}

// ---------------------------------------------------------------------------
// Filter construction helpers
// ---------------------------------------------------------------------------

/**
 * Create an IN filter if values are provided, otherwise returns empty array.
 */
export function inFilter(
  field: string,
  values: ReadonlyArray<string | number> | undefined,
): Filter[] {
  if (values === undefined || values.length === 0) return [];
  return [{field, op: 'in', value: [...values]}];
}

/**
 * Create range filters (>= min, <= max) if range is provided.
 */
export function rangeFilters(
  field: string,
  range: {readonly min: number; readonly max: number} | undefined,
): Filter[] {
  if (range === undefined) return [];
  return [
    {field, op: '>=', value: range.min},
    {field, op: '<=', value: range.max},
  ];
}
