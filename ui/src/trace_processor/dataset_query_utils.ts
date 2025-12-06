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

  // Return the query plan
  return {
    queries: plannedQueries,

    async execute(
      engine: Engine,
    ): Promise<readonly QueryWithLineageResult<T, Schema>[]> {
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
