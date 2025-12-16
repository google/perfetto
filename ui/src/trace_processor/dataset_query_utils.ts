// Copyright (C) 2025 The Android Open Source Project
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

import {DatasetSchema, SourceDataset, UnionDataset} from './dataset';
import {NUM, SqlValue} from './query_result';
import {Engine} from './engine';

/**
 * Configuration for queryWithLineage function.
 */
export interface QueryWithLineageConfig<T, Schema extends DatasetSchema> {
  /** The list of input objects (e.g., tracks) */
  inputs: T[];

  /** Function to extract dataset from each input */
  datasetFetcher: (input: T) => SourceDataset;

  /** Columns to select from the union */
  columns: Schema;

  /**
   * Optional additional columns needed for filtering but not in result.
   * These columns will be available in the base query for the queryBuilder
   * to reference in WHERE clauses, but won't be included in the final result.
   */
  filterColumns?: DatasetSchema;

  /**
   * Skip partition filters (e.g., track_id IN (...)) when true.
   * This is useful for search operations where you want to search across
   * all data from the source table without partition filtering.
   * The lineage will still be tracked.
   * Default: false
   */
  skipPartitionFilters?: boolean;

  /**
   * When true, executes all source groups in a single SQL query using UNION ALL
   * with a z__groupid column for lineage resolution. This is more efficient when
   * querying many source groups as it reduces round-trips to SQLite.
   * Default: false (executes separate queries per source group)
   */
  singleQuery?: boolean;

  /**
   * Optional query builder to wrap the union.
   * @param baseQuery - The optimized union query with result + partition + filter columns
   * @param resultCols - The columns that should be selected in the final result
   * Use this to add WHERE clauses, joins, or other SQL operations.
   */
  queryBuilder?: (baseQuery: string, resultCols: string[]) => string;
}

/**
 * Result from queryWithLineage with source tracking.
 */
export interface QueryWithLineageResult<T, Schema extends DatasetSchema> {
  /** The source input object this row came from */
  readonly source: T;
  /** The row data matching the requested schema */
  readonly row: Schema;
}

/**
 * Internal: Partition map for tracking which inputs map to which partition values.
 */
interface PartitionMap<T> {
  /** For each partition column, map values to source inputs */
  columns: Map<string, Map<SqlValue, Set<T>>>;
}

/**
 * Internal: Group of inputs that share the same source dataset.
 */
interface SourceGroup<T> {
  input: T;
  dataset: SourceDataset;
}

/**
 * Information about a planned query for a source group.
 */
export interface PlannedQuery<T = unknown> {
  /** The source SQL statement/table being queried */
  readonly src: string;
  /** The inputs that belong to this group */
  readonly inputs: ReadonlyArray<T>;
  /** The generated SQL query */
  readonly sql: string;
  /** The partition columns used for lineage tracking */
  readonly partitionColumns: ReadonlyArray<string>;
  /** Map from partition column name to (value -> inputs) for lineage resolution */
  readonly partitionMap: ReadonlyMap<
    string,
    ReadonlyMap<SqlValue, ReadonlySet<T>>
  >;
}

/**
 * A lineage resolver that maps (groupid, partition) -> input.
 * Used to resolve track lineage from query results.
 */
export interface LineageResolver<T> {
  /**
   * Resolve the source input(s) for a given groupid and partition value.
   * Returns undefined if no match is found.
   */
  resolve(groupId: number, partitionValue: SqlValue): T | undefined;

  /**
   * Get all inputs that belong to a specific group.
   */
  getGroupInputs(groupId: number): ReadonlyArray<T>;
}

/**
 * Result from buildQueryWithLineage containing SQL and lineage resolver.
 */
export interface QueryWithLineage<T> {
  /** The SQL query string with z__groupid and z__partition columns */
  readonly sql: string;
  /** Resolver to map (groupid, partition) back to source inputs */
  readonly lineageResolver: LineageResolver<T>;
  /** The columns that are selected (excluding z__groupid and z__partition) */
  readonly columns: ReadonlyArray<string>;
}

/**
 * A query plan that can be inspected before execution.
 * Useful for testing and debugging.
 */
export interface QueryPlan<T, Schema extends DatasetSchema> {
  /** Information about each planned query */
  readonly queries: ReadonlyArray<PlannedQuery<T>>;

  /** Execute the plan and return results with lineage */
  execute(
    engine: Engine,
  ): Promise<readonly QueryWithLineageResult<T, Schema>[]>;
}

/**
 * Build partition map from a group of datasets.
 * Maps partition column values back to source inputs for O(1) lineage lookup.
 * Also tracks inputs without filters (unpartitioned inputs).
 */
interface PartitionMapWithUnfiltered<T> extends PartitionMap<T> {
  /** Inputs without filters that should match all rows */
  unfilteredInputs: Set<T>;
}

function buildPartitionMap<T>(
  group: Array<SourceGroup<T>>,
): PartitionMapWithUnfiltered<T> {
  const partitionMap: PartitionMapWithUnfiltered<T> = {
    columns: new Map(),
    unfilteredInputs: new Set(),
  };

  for (const {input, dataset} of group) {
    if (!dataset.filter) {
      // Track inputs without filters - they match all rows
      partitionMap.unfilteredInputs.add(input);
      continue;
    }

    const colName = dataset.filter.col;
    const valueMap = partitionMap.columns.get(colName) ?? new Map();
    partitionMap.columns.set(colName, valueMap);

    const values =
      'eq' in dataset.filter ? [dataset.filter.eq] : dataset.filter.in;

    for (const value of values) {
      const sourceSet = valueMap.get(value) ?? new Set();
      sourceSet.add(input);
      valueMap.set(value, sourceSet);
    }
  }

  return partitionMap;
}

const CTE_CHUNK_SIZE = 500;

/**
 * Builds a query from an array of subqueries using UNION ALL.
 * If over CTE_CHUNK_SIZE items, breaks into CTEs to help the query parser.
 */
function buildUnionAllQuery(queries: string[]): string {
  if (queries.length === 0) {
    throw new Error('Cannot build union from empty query list');
  }
  if (queries.length === 1) {
    return queries[0];
  }
  if (queries.length <= CTE_CHUNK_SIZE) {
    return queries.join('\nUNION ALL\n');
  }

  // Break into chunks and create CTEs
  const ctes: string[] = [];
  const cteNames: string[] = [];

  for (let i = 0; i < queries.length; i += CTE_CHUNK_SIZE) {
    const chunk = queries.slice(i, i + CTE_CHUNK_SIZE);
    const cteName = `_chunk_${Math.floor(i / CTE_CHUNK_SIZE)}`;
    cteNames.push(cteName);
    ctes.push(`${cteName} AS (\n${chunk.join('\nUNION ALL\n')}\n)`);
  }

  const cteSection = `WITH ${ctes.join(',\n')}`;
  const finalUnion = cteNames
    .map((name) => `SELECT * FROM ${name}`)
    .join('\nUNION ALL\n');

  return `${cteSection}\n${finalUnion}`;
}

/**
 * Build union dataset for a source group, optionally skipping partition filters.
 */
function buildUnionDataset<T>(
  group: Array<SourceGroup<T>>,
  skipPartitionFilters: boolean,
): UnionDataset {
  if (skipPartitionFilters) {
    // Create datasets without filters for unfiltered search
    const unfilteredDatasets = group.map((g) => {
      return new SourceDataset({
        src: g.dataset.src,
        schema: g.dataset.schema,
        select: g.dataset.select,
        joins: g.dataset.joins,
        // No filter - query entire table
      });
    });
    return UnionDataset.create(unfilteredDatasets);
  } else {
    // Use original datasets with partition filters
    const datasets = group.map((g) => g.dataset);
    return UnionDataset.create(datasets);
  }
}

/**
 * Query a single source group (datasets with same src) and resolve lineage.
 */
async function querySourceGroup<T, Schema extends DatasetSchema>(
  engine: Engine,
  group: Array<SourceGroup<T>>,
  unionDataset: UnionDataset,
  partitionMap: PartitionMapWithUnfiltered<T>,
  columns: Schema,
  filterColumns: DatasetSchema | undefined,
  queryBuilder?: (baseQuery: string, resultCols: string[]) => string,
): Promise<Array<QueryWithLineageResult<T, Schema>>> {
  // Include partition columns in query schema
  const partitionColumns = Array.from(partitionMap.columns.keys());

  // Build query schema: result columns + partition columns + filter columns
  const querySchema: DatasetSchema = {
    ...columns,
    ...Object.fromEntries(partitionColumns.map((col) => [col, NUM])),
    ...(filterColumns ?? {}),
  };

  // Query with only the columns we need (enables column & join elimination)
  const baseQuery = unionDataset.query(querySchema);

  // Build query with optional custom query builder
  const resultCols = [...Object.keys(columns), ...partitionColumns];
  const query = queryBuilder ? queryBuilder(baseQuery, resultCols) : baseQuery;

  // Execute query
  const result = await engine.query(query);

  // Build schema for result iteration (columns + partitions)
  const resultSchema: DatasetSchema = {
    ...columns,
    ...Object.fromEntries(partitionColumns.map((col) => [col, NUM])),
  };

  // Process results and resolve lineage
  const results: Array<QueryWithLineageResult<T, Schema>> = [];

  for (
    const iter = result.iter(resultSchema);
    iter.valid() === true;
    iter.next()
  ) {
    // Extract row data
    const row: Record<string, SqlValue> = {};
    for (const col of Object.keys(columns)) {
      row[col] = iter.get(col);
    }

    // Resolve source via partition values
    let sources: Set<T> = new Set();

    if (partitionColumns.length === 0) {
      // No partition columns - this is non-partitioned data
      // All inputs in the group contributed to this result
      sources = new Set(group.map((g) => g.input));
    } else {
      // Try to find matching sources via partition values
      for (const colName of partitionColumns) {
        const partitionValue = iter.get(colName);
        const valueMap = partitionMap.columns.get(colName);
        const matchingSources = valueMap?.get(partitionValue);

        if (matchingSources) {
          // Add all inputs that match this partition value
          for (const source of matchingSources) {
            sources.add(source);
          }
          break;
        }
      }

      // Also add unfiltered inputs - they match ALL rows
      for (const unfilteredInput of partitionMap.unfilteredInputs) {
        sources.add(unfilteredInput);
      }

      // If no sources matched at all, skip this row
      if (sources.size === 0) {
        continue;
      }
    }

    // Push one result per source
    for (const source of sources) {
      results.push({source, row: row as Schema});
    }
  }

  return results;
}

/**
 * This function analyzes input datasets and generates optimized SQL queries by:
 * 1. Grouping inputs by their source (e.g., all tracks from 'slice')
 * 2. Merging filters into efficient IN clauses (e.g., WHERE track_id IN (1,2,3))
 * 3. Including partition columns for lineage tracking
 * 4. Eliminating unused joins and columns for performance
 *
 * Each generated query has the structure:
 * ```sql
 * SELECT <result_columns>, <partition_columns> FROM <source_table>
 * [JOIN <tables>]
 * WHERE <partition_col> IN (<values>)
 * [AND <custom_filters>]
 * ```
 *
 * For example, querying 3 tracks might produce:
 * ```sql
 * SELECT id, ts, track_id FROM slice WHERE track_id IN (1,2,3)
 * ```
 *
 * The partition columns (like `track_id`) enable O(1) lineage resolution - mapping
 * each result row back to its source input by matching partition values.
 *
 * @param config - Configuration including inputs, dataset fetcher, columns, and optional query builder
 * @returns A query plan that can be inspected or executed
 *
 * @example
 * ```ts
 * const plan = planQuery({
 *   inputs: tracks,
 *   datasetFetcher: (track) => track.renderer.getDataset?.(),
 *   columns: {id: NUM, ts: LONG},
 *   queryBuilder: (q, cols) => `SELECT ${cols.join(', ')} FROM (${q}) WHERE name LIKE '%foo%'`,
 * });
 *
 * // Inspect the plan
 * console.log('Groups:', plan.groupCount);
 * console.log('Total inputs:', plan.totalInputs);
 * for (const query of plan.queries) {
 *   console.log('Source:', query.src);
 *   console.log('SQL:', query.sql);
 *   console.log('Inputs:', query.inputs.map(i => i.id));
 * }
 *
 * // Execute when ready
 * const results = await plan.execute(engine);
 * ```
 */
export function planQuery<T, Schema extends DatasetSchema>(
  config: QueryWithLineageConfig<T, Schema>,
): QueryPlan<T, Schema> {
  // Group by source (SQL statement/table)
  const sourceGroups = new Map<string, Array<SourceGroup<T>>>();

  for (const input of config.inputs) {
    const dataset = config.datasetFetcher(input);
    const group = sourceGroups.get(dataset.src) ?? [];
    group.push({input, dataset});
    sourceGroups.set(dataset.src, group);
  }

  // Build plan information for each source group
  const plannedQueries: PlannedQuery<T>[] = [];
  const unionDatasets = new Map<string, UnionDataset>();
  const partitionMaps = new Map<string, PartitionMapWithUnfiltered<T>>();

  for (const [src, group] of sourceGroups.entries()) {
    // Build and cache partition map
    const partitionMap = buildPartitionMap(group);
    partitionMaps.set(src, partitionMap);
    const partitionColumns = Array.from(partitionMap.columns.keys());

    // Build union dataset once and cache it for execution
    const unionDataset = buildUnionDataset(
      group,
      config.skipPartitionFilters ?? false,
    );
    unionDatasets.set(src, unionDataset);

    // Build query schema: result + partition + filter columns
    const querySchema: DatasetSchema = {
      ...config.columns,
      ...Object.fromEntries(partitionColumns.map((col) => [col, NUM])),
      ...(config.filterColumns ?? {}),
    };

    // Query with only needed columns (enables column & join elimination)
    const baseQuery = unionDataset.query(querySchema);

    // Select only result + partition columns
    const resultCols = [...Object.keys(config.columns), ...partitionColumns];

    // Apply query builder or use the base query directly (no need to wrap)
    const sql = config.queryBuilder
      ? config.queryBuilder(baseQuery, resultCols)
      : baseQuery;

    // Convert partition map to readonly format for external exposure
    const readonlyPartitionMap = new Map<
      string,
      ReadonlyMap<SqlValue, ReadonlySet<T>>
    >();
    for (const [col, valueMap] of partitionMap.columns.entries()) {
      readonlyPartitionMap.set(col, valueMap);
    }

    plannedQueries.push({
      src,
      inputs: group.map((g) => g.input),
      sql,
      partitionColumns,
      partitionMap: readonlyPartitionMap,
    });
  }

  // Build array of source groups indexed by groupid for single query mode
  const sourceGroupArray = Array.from(sourceGroups.entries());

  // Return the query plan
  return {
    queries: plannedQueries,

    async execute(
      engine: Engine,
    ): Promise<readonly QueryWithLineageResult<T, Schema>[]> {
      // Single query mode: UNION ALL groups with z__groupid and z__partition
      if ((config.singleQuery ?? true) && sourceGroupArray.length > 0) {
        return executeSingleQuery(
          engine,
          sourceGroupArray,
          unionDatasets,
          partitionMaps,
          config.columns,
          config.filterColumns,
          config.queryBuilder,
        );
      }

      // Multi-query mode: execute each source group separately
      let allResults: readonly QueryWithLineageResult<T, Schema>[] = [];

      for (const [src, group] of sourceGroups.entries()) {
        const partitionMap = partitionMaps.get(src)!;
        const unionDataset = unionDatasets.get(src)!;
        const groupResults = await querySourceGroup(
          engine,
          group,
          unionDataset,
          partitionMap,
          config.columns,
          config.filterColumns,
          config.queryBuilder,
        );
        allResults = allResults.concat(groupResults);
      }

      return allResults;
    },
  };
}

/**
 * Execute all source groups in a single SQL query using UNION ALL with
 * z__groupid and z__partition columns for lineage resolution.
 */
async function executeSingleQuery<T, Schema extends DatasetSchema>(
  engine: Engine,
  sourceGroupArray: Array<[string, Array<SourceGroup<T>>]>,
  unionDatasets: Map<string, UnionDataset>,
  partitionMaps: Map<string, PartitionMapWithUnfiltered<T>>,
  columns: Schema,
  filterColumns: DatasetSchema | undefined,
  queryBuilder?: (baseQuery: string, resultCols: string[]) => string,
): Promise<readonly QueryWithLineageResult<T, Schema>[]> {
  const resultColNames = Object.keys(columns);
  const filterColNames = filterColumns ? Object.keys(filterColumns) : [];

  // Build per-group queries with normalized z__groupid and z__partition columns
  const groupQueries: string[] = [];

  for (let groupId = 0; groupId < sourceGroupArray.length; groupId++) {
    const [src, _group] = sourceGroupArray[groupId];
    const unionDataset = unionDatasets.get(src)!;
    const partitionMap = partitionMaps.get(src)!;
    const partitionColumns = Array.from(partitionMap.columns.keys());

    // Build query schema: result columns + partition columns + filter columns
    const querySchema: DatasetSchema = {
      ...columns,
      ...Object.fromEntries(partitionColumns.map((col) => [col, NUM])),
      ...(filterColumns ?? {}),
    };

    const baseQuery = unionDataset.query(querySchema);

    // Normalize partition to single z__partition column (use first partition col or NULL)
    const partitionExpr =
      partitionColumns.length > 0 ? partitionColumns[0] : 'NULL';

    // Select result columns + filter columns + groupid + partition
    // Filter columns must be included so queryBuilder can reference them in WHERE clauses
    const selectCols = [...resultColNames, ...filterColNames].join(', ');
    groupQueries.push(
      `SELECT ${selectCols}, ${groupId} AS z__groupid, ${partitionExpr} AS z__partition FROM (${baseQuery})`,
    );
  }

  // Combine all groups with UNION ALL
  const combinedQuery = buildUnionAllQuery(groupQueries);

  // Apply query builder if provided
  // Include filter columns so they're available for WHERE clauses
  const resultCols = [
    ...resultColNames,
    ...filterColNames,
    'z__groupid',
    'z__partition',
  ];
  const finalQuery = queryBuilder
    ? queryBuilder(combinedQuery, resultCols)
    : combinedQuery;

  // Execute the combined query
  const result = await engine.query(finalQuery);

  // Build schema for result iteration
  const resultSchema: DatasetSchema = {
    ...columns,
    z__groupid: NUM,
    z__partition: NUM,
  };

  // Process results and resolve lineage
  const results: Array<QueryWithLineageResult<T, Schema>> = [];

  for (
    const iter = result.iter(resultSchema);
    iter.valid() === true;
    iter.next()
  ) {
    // Extract row data (without z__groupid and z__partition)
    const row: Record<string, SqlValue> = {};
    for (const col of resultColNames) {
      row[col] = iter.get(col);
    }

    // Get groupid and partition for lineage resolution
    const groupId = iter.get('z__groupid') as number;
    const partitionValue = iter.get('z__partition');

    // Look up the source group
    const [src, group] = sourceGroupArray[groupId];
    const partitionMap = partitionMaps.get(src)!;

    // Resolve source via partition value
    let sources: Set<T> = new Set();

    if (partitionMap.columns.size === 0) {
      // No partition columns - all inputs in group match
      sources = new Set(group.map((g) => g.input));
    } else {
      // Find matching sources via partition value
      for (const [_colName, valueMap] of partitionMap.columns.entries()) {
        const matchingSources = valueMap.get(partitionValue);
        if (matchingSources) {
          for (const source of matchingSources) {
            sources.add(source);
          }
          break;
        }
      }

      // Also add unfiltered inputs - they match ALL rows
      for (const unfilteredInput of partitionMap.unfilteredInputs) {
        sources.add(unfilteredInput);
      }

      // Skip row if no sources matched
      if (sources.size === 0) {
        continue;
      }
    }

    // Push one result per source
    for (const source of sources) {
      results.push({source, row: row as Schema});
    }
  }

  return results;
}

/**
 * Configuration for buildQueryWithLineage.
 */
export interface BuildQueryWithLineageConfig<T, Schema extends DatasetSchema> {
  /** The list of input objects (e.g., tracks) */
  inputs: T[];

  /** Function to extract dataset from each input */
  datasetFetcher: (input: T) => SourceDataset;

  /** Columns to select from the union */
  columns: Schema;

  /**
   * Optional additional columns needed for filtering but not in result.
   */
  filterColumns?: DatasetSchema;
}

/**
 * Builds a SQL query with z__groupid and z__partition columns for lineage tracking.
 * Returns the SQL string and a resolver to map results back to source inputs.
 *
 * This is useful for aggregation panels that need to create SQL tables with
 * lineage information, then resolve track URIs when rendering clickable IDs.
 *
 * @example
 * ```ts
 * const {sql, lineageResolver, columns} = buildQueryWithLineage({
 *   inputs: tracks,
 *   datasetFetcher: (t) => t.renderer.getDataset?.(),
 *   columns: {id: NUM, ts: LONG, dur: LONG, name: STR_NULL},
 * });
 *
 * // Create table with lineage columns
 * await engine.query(`CREATE TABLE agg AS ${sql}`);
 *
 * // Later, when rendering a row:
 * const track = lineageResolver.resolve(row.z__groupid, row.z__partition);
 * if (track) {
 *   trace.selection.selectTrackEvent(track.uri, row.id);
 * }
 * ```
 */
export function buildQueryWithLineage<T, Schema extends DatasetSchema>(
  config: BuildQueryWithLineageConfig<T, Schema>,
): QueryWithLineage<T> {
  const resultColNames = Object.keys(config.columns);
  const filterColNames = config.filterColumns
    ? Object.keys(config.filterColumns)
    : [];

  // Group by source (SQL statement/table)
  const sourceGroups = new Map<string, Array<SourceGroup<T>>>();

  for (const input of config.inputs) {
    const dataset = config.datasetFetcher(input);
    const group = sourceGroups.get(dataset.src) ?? [];
    group.push({input, dataset});
    sourceGroups.set(dataset.src, group);
  }

  const sourceGroupArray = Array.from(sourceGroups.entries());

  // Build partition maps for lineage resolution
  const partitionMaps = new Map<string, PartitionMapWithUnfiltered<T>>();
  for (const [src, group] of sourceGroupArray) {
    partitionMaps.set(src, buildPartitionMap(group));
  }

  // Build per-group queries with z__groupid and z__partition columns
  const groupQueries: string[] = [];

  for (let groupId = 0; groupId < sourceGroupArray.length; groupId++) {
    const [src, group] = sourceGroupArray[groupId];
    const partitionMap = partitionMaps.get(src)!;
    const partitionColumns = Array.from(partitionMap.columns.keys());

    // Build union dataset for this group
    const datasets = group.map((g) => g.dataset);
    const unionDataset = UnionDataset.create(datasets);

    // Build query schema: result columns + partition columns + filter columns
    const querySchema: DatasetSchema = {
      ...config.columns,
      ...Object.fromEntries(partitionColumns.map((col) => [col, NUM])),
      ...(config.filterColumns ?? {}),
    };

    const baseQuery = unionDataset.query(querySchema);

    // Normalize partition to single z__partition column
    const partitionExpr =
      partitionColumns.length > 0 ? partitionColumns[0] : 'NULL';

    // Select result columns + filter columns + groupid + partition
    const selectCols = [...resultColNames, ...filterColNames].join(', ');
    groupQueries.push(
      `SELECT ${selectCols}, ${groupId} AS z__groupid, ${partitionExpr} AS z__partition FROM (${baseQuery})`,
    );
  }

  // Handle empty case
  if (groupQueries.length === 0) {
    // Return empty query with proper columns
    const cols = [...resultColNames, ...filterColNames].join(', ');
    return {
      sql: `SELECT ${cols}, 0 AS z__groupid, NULL AS z__partition WHERE FALSE`,
      columns: resultColNames,
      lineageResolver: {
        resolve: () => undefined,
        getGroupInputs: () => [],
      },
    };
  }

  // Combine all groups with UNION ALL
  const sql = buildUnionAllQuery(groupQueries);

  // Build lineage resolver
  const lineageResolver: LineageResolver<T> = {
    resolve(groupId: number, partitionValue: SqlValue): T | undefined {
      if (groupId < 0 || groupId >= sourceGroupArray.length) {
        return undefined;
      }

      const [src, group] = sourceGroupArray[groupId];
      const partitionMap = partitionMaps.get(src)!;

      // No partition columns - return first input in group
      if (partitionMap.columns.size === 0) {
        return group[0]?.input;
      }

      // Find matching source via partition value
      for (const [_colName, valueMap] of partitionMap.columns.entries()) {
        const matchingSources = valueMap.get(partitionValue);
        if (matchingSources && matchingSources.size > 0) {
          // Return first matching source
          return matchingSources.values().next().value;
        }
      }

      // Check unfiltered inputs
      if (partitionMap.unfilteredInputs.size > 0) {
        return partitionMap.unfilteredInputs.values().next().value;
      }

      return undefined;
    },

    getGroupInputs(groupId: number): ReadonlyArray<T> {
      if (groupId < 0 || groupId >= sourceGroupArray.length) {
        return [];
      }
      const [_src, group] = sourceGroupArray[groupId];
      return group.map((g) => g.input);
    },
  };

  return {
    sql,
    lineageResolver,
    columns: resultColNames,
  };
}
