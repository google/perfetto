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

import protos from '../../../protos';
import {nextNodeId, QueryNode} from '../query_node';

/**
 * Type representing a QueryNode.
 * Builder methods accept nodes directly and extract their queries internally.
 */
export type QuerySource = QueryNode | undefined;

/**
 * Helper function to extract a structured query from a QuerySource.
 * @param source The query source (node)
 * @returns The structured query, or undefined if extraction fails
 */
function extractQuery(
  source: QuerySource,
): protos.PerfettoSqlStructuredQuery | undefined {
  if (source === undefined) return undefined;
  return source.getStructuredQuery();
}

/**
 * Sorting criterion for ORDER BY clauses
 */
export interface SortCriterion {
  columnName: string;
  direction: 'ASC' | 'DESC';
}

/**
 * Aggregation specification for GROUP BY
 */
export interface AggregationSpec {
  columnName?: string; // Optional for COUNT(*)
  op: string; // e.g., 'SUM', 'COUNT', 'AVG', etc.
  resultColumnName?: string;
  percentile?: number; // Required for PERCENTILE operation (0-100)
}

/**
 * Column selection specification
 */
export interface ColumnSpec {
  columnNameOrExpression: string;
  alias?: string;
  referencedModule?: string;
}

/**
 * Join condition types
 */
export interface JoinCondition {
  type: 'equality' | 'freeform';
  leftColumn?: string;
  rightColumn?: string;
  leftQueryAlias?: string;
  rightQueryAlias?: string;
  sqlExpression?: string;
}

/**
 * SQL dependency specification
 */
export interface SqlDependency {
  alias: string;
  query: protos.PerfettoSqlStructuredQuery | undefined;
}

/**
 * Service responsible for creating PerfettoSqlStructuredQuery objects
 * with proper id assignment and nesting prevention.
 *
 * This service ensures that:
 * - All created queries have proper ids
 * - Filters are applied to inner queries when appropriate
 * - Unnecessary query nesting is avoided
 * - Nodes don't need to import or create protobuf objects
 */
export class StructuredQueryBuilder {
  /**
   * Creates a new structured query with innerQuery wrapper.
   * Automatically assigns an id to prevent nesting issues.
   *
   * @param innerQuery The query to wrap
   * @param nodeId Optional node id. If not provided, generates a new one.
   * @returns A new structured query wrapping the inner query
   */
  static wrapWithInnerQuery(
    innerQuery: protos.PerfettoSqlStructuredQuery,
    nodeId?: string,
  ): protos.PerfettoSqlStructuredQuery {
    const sq = new protos.PerfettoSqlStructuredQuery();
    sq.id = nodeId ?? nextNodeId();
    sq.innerQuery = innerQuery;
    return sq;
  }

  /**
   * Applies column selection from a node's finalCols to a structured query.
   * Mutates the query in place. If all columns are selected, does nothing
   * (no explicit column selection needed).
   *
   * @param sq The structured query to modify
   * @param node The node whose finalCols define the column selection
   */
  static applyNodeColumnSelection(
    sq: protos.PerfettoSqlStructuredQuery,
    node: QueryNode,
  ): void {
    // If all columns are selected, no explicit selection needed
    if (node.finalCols.every((c) => c.checked)) return;

    sq.selectColumns = node.finalCols
      .filter((c) => c.checked !== false)
      .map((c) => {
        const col = new protos.PerfettoSqlStructuredQuery.SelectColumn();
        col.columnName = c.column.name;
        if (c.alias) col.alias = c.alias;
        return col;
      });
  }

  /**
   * Creates a structured query with ORDER BY clause.
   * Wraps the inner query and adds the orderBy specification.
   *
   * @param innerQuery The query to sort (can be a QueryNode or structured query)
   * @param criteria Array of sort criteria (column names and directions)
   * @param nodeId The node id to assign
   * @returns A new structured query with ORDER BY, or undefined if extraction fails
   */
  static withOrderBy(
    innerQuery: QuerySource,
    criteria: SortCriterion[],
    nodeId: string,
  ): protos.PerfettoSqlStructuredQuery | undefined {
    const query = extractQuery(innerQuery);
    if (!query) return undefined;

    const orderingSpecs: protos.PerfettoSqlStructuredQuery.OrderBy.IOrderingSpec[] =
      criteria.map((c) => ({
        columnName: c.columnName,
        direction:
          c.direction === 'DESC'
            ? protos.PerfettoSqlStructuredQuery.OrderBy.Direction.DESC
            : protos.PerfettoSqlStructuredQuery.OrderBy.Direction.ASC,
      }));

    return protos.PerfettoSqlStructuredQuery.create({
      id: nodeId,
      innerQuery: query,
      orderBy: protos.PerfettoSqlStructuredQuery.OrderBy.create({
        orderingSpecs,
      }),
    });
  }

  /**
   * Creates a structured query with LIMIT and/or OFFSET.
   * Wraps the inner query and adds limit/offset.
   *
   * @param innerQuery The query to limit (can be a QueryNode or structured query)
   * @param limit Optional limit value
   * @param offset Optional offset value
   * @param nodeId The node id to assign
   * @returns A new structured query with LIMIT/OFFSET, or undefined if extraction fails
   */
  static withLimitOffset(
    innerQuery: QuerySource,
    limit?: number,
    offset?: number,
    nodeId?: string,
  ): protos.PerfettoSqlStructuredQuery | undefined {
    const query = extractQuery(innerQuery);
    if (!query) return undefined;

    const sq = new protos.PerfettoSqlStructuredQuery();
    sq.id = nodeId ?? nextNodeId();
    sq.innerQuery = query;

    if (limit !== undefined && limit >= 0) {
      sq.limit = limit;
    }

    if (offset !== undefined && offset > 0) {
      sq.offset = offset;
    }

    return sq;
  }

  /**
   * Creates a structured query with interval intersect operation.
   *
   * @param baseQuery The base query for the intersection
   * @param intervalQueries Array of interval queries to intersect with the base
   * @param partitionColumns Optional partition columns for the intersection
   * @param filterNegativeDur Optional array of booleans indicating which queries should filter dur >= 0
   * @param nodeId The node id to assign
   * @returns A new structured query with interval intersect, or undefined if extraction fails
   */
  static withIntervalIntersect(
    baseQuery: QuerySource,
    intervalQueries: QuerySource[],
    partitionColumns?: string[],
    filterNegativeDur?: boolean[],
    nodeId?: string,
  ): protos.PerfettoSqlStructuredQuery | undefined {
    // Extract and optionally filter base query
    let base = extractQuery(baseQuery);
    if (!base) return undefined;
    if (filterNegativeDur && filterNegativeDur[0]) {
      base = this.applyDurFilter(base);
    }

    // Extract and optionally filter interval queries
    const intervals: protos.PerfettoSqlStructuredQuery[] = [];
    for (let i = 0; i < intervalQueries.length; i++) {
      let query = extractQuery(intervalQueries[i]);
      if (!query) return undefined;
      if (filterNegativeDur && filterNegativeDur[i + 1]) {
        query = this.applyDurFilter(query);
      }
      intervals.push(query);
    }

    const sq = new protos.PerfettoSqlStructuredQuery();
    sq.id = nodeId ?? nextNodeId();
    sq.intervalIntersect =
      new protos.PerfettoSqlStructuredQuery.IntervalIntersect();
    sq.intervalIntersect.base = base;
    sq.intervalIntersect.intervalIntersect = intervals;

    if (partitionColumns && partitionColumns.length > 0) {
      sq.intervalIntersect.partitionColumns = [...partitionColumns];
    }

    return sq;
  }

  /**
   * Applies a dur >= 0 filter to a structured query.
   * The filter is applied to the inner query if present to avoid wrapping.
   * This is a private helper method used internally by builder methods.
   *
   * @param sq The structured query to filter
   * @returns The modified structured query (mutates in place)
   */
  private static applyDurFilter(
    sq: protos.PerfettoSqlStructuredQuery,
  ): protos.PerfettoSqlStructuredQuery {
    // Apply filter to the inner query if it exists, otherwise to the base
    const targetSq = sq.innerQuery ? sq.innerQuery : sq;

    // Create the dur >= 0 filter
    const filter = new protos.PerfettoSqlStructuredQuery.Filter();
    filter.columnName = 'dur';
    filter.op =
      protos.PerfettoSqlStructuredQuery.Filter.Operator.GREATER_THAN_EQUAL;
    filter.int64Rhs = [0];

    // Add the filter
    if (!targetSq.filters) targetSq.filters = [];
    targetSq.filters.push(filter);

    // Ensure the target query has an id to prevent nesting issues
    if (!targetSq.id) {
      targetSq.id = nextNodeId();
    }

    return sq;
  }

  /**
   * Creates a structured query with GROUP BY and aggregations.
   * Automatically wraps the query in an inner query if it already has a GROUP BY
   * or selectColumns (to ensure aliases are in scope).
   *
   * @param innerQuery The query to group
   * @param groupByColumns Column names to group by
   * @param aggregations Array of aggregation specifications
   * @param nodeId The node id to assign
   * @returns A new structured query with GROUP BY, or undefined if extraction fails
   */
  static withGroupBy(
    innerQuery: QuerySource,
    groupByColumns: string[],
    aggregations: AggregationSpec[],
    nodeId: string,
  ): protos.PerfettoSqlStructuredQuery | undefined {
    let query = extractQuery(innerQuery);
    if (!query) return undefined;

    // If the query already has a GROUP BY or selectColumns, wrap it in an inner query
    // This ensures that aliases from SELECT are available in GROUP BY scope
    if (query.groupBy !== undefined || (query.selectColumns?.length ?? 0) > 0) {
      query = this.wrapWithInnerQuery(query);
    }

    const groupByProto = new protos.PerfettoSqlStructuredQuery.GroupBy();
    groupByProto.columnNames = groupByColumns;

    groupByProto.aggregates = aggregations.map((agg) => {
      const aggProto =
        new protos.PerfettoSqlStructuredQuery.GroupBy.Aggregate();

      // columnName is optional for COUNT(*)
      if (agg.columnName) {
        aggProto.columnName = agg.columnName;
      }

      aggProto.op =
        protos.PerfettoSqlStructuredQuery.GroupBy.Aggregate.Op[
          agg.op as keyof typeof protos.PerfettoSqlStructuredQuery.GroupBy.Aggregate.Op
        ];

      if (agg.resultColumnName) {
        aggProto.resultColumnName = agg.resultColumnName;
      }

      // percentile is required for PERCENTILE operation
      if (agg.percentile !== undefined) {
        aggProto.percentile = agg.percentile;
      }

      return aggProto;
    });

    query.groupBy = groupByProto;
    query.id = nodeId;
    return query;
  }

  /**
   * Creates a structured query with column selection.
   *
   * @param innerQuery The query to select from (can be a QueryNode or structured query)
   * @param columns Array of column specifications
   * @param referencedModules Optional array of referenced module names
   * @param nodeId The node id to assign
   * @returns A new structured query with column selection, or undefined if extraction fails
   */
  static withSelectColumns(
    innerQuery: QuerySource,
    columns: ColumnSpec[],
    referencedModules?: string[],
    nodeId?: string,
  ): protos.PerfettoSqlStructuredQuery | undefined {
    let query = extractQuery(innerQuery);
    if (!query) return undefined;

    // If the query already has selectColumns, wrap it in an inner query
    // to ensure we create a new query object (so changes are detected)
    if ((query.selectColumns?.length ?? 0) > 0) {
      query = this.wrapWithInnerQuery(query);
    }

    query.selectColumns = columns.map((col) => {
      const selectCol = new protos.PerfettoSqlStructuredQuery.SelectColumn();
      selectCol.columnNameOrExpression = col.columnNameOrExpression;
      if (col.alias && col.alias.trim() !== '') {
        selectCol.alias = col.alias;
      }
      return selectCol;
    });

    if (referencedModules && referencedModules.length > 0) {
      query.referencedModules = referencedModules;
    }

    if (nodeId) {
      query.id = nodeId;
    }

    return query;
  }

  /**
   * Creates a structured query from a table source.
   *
   * @param tableName The name of the table
   * @param moduleName Optional module name for the table
   * @param columnNames Optional array of column names to include
   * @param nodeId The node id to assign
   * @returns A new structured query for the table
   */
  static fromTable(
    tableName: string,
    moduleName?: string,
    columnNames?: string[],
    nodeId?: string,
  ): protos.PerfettoSqlStructuredQuery {
    const sq = new protos.PerfettoSqlStructuredQuery();
    sq.id = nodeId ?? nextNodeId();
    sq.table = new protos.PerfettoSqlStructuredQuery.Table();
    sq.table.tableName = tableName;
    if (moduleName) {
      sq.table.moduleName = moduleName;
    }
    if (columnNames) {
      sq.table.columnNames = columnNames;
    }
    return sq;
  }

  /**
   * Creates a structured query from SQL.
   *
   * @param sql The SQL query string
   * @param dependencies Array of query dependencies
   * @param columnNames Array of column names in the result
   * @param nodeId The node id to assign
   * @returns A new structured query for the SQL
   */
  static fromSql(
    sql: string,
    dependencies: SqlDependency[],
    columnNames: string[],
    nodeId?: string,
  ): protos.PerfettoSqlStructuredQuery {
    const sq = new protos.PerfettoSqlStructuredQuery();
    sq.id = nodeId ?? nextNodeId();

    const sqlProto = new protos.PerfettoSqlStructuredQuery.Sql();
    sqlProto.sql = sql;
    sqlProto.columnNames = columnNames;

    sqlProto.dependencies = dependencies.map((dep) => {
      const depProto = new protos.PerfettoSqlStructuredQuery.Sql.Dependency();
      depProto.alias = dep.alias;
      depProto.query = dep.query;
      return depProto;
    });

    sq.sql = sqlProto;
    return sq;
  }

  /**
   * Creates a structured query from a time range.
   * Produces a single-row result with columns: id (always 0), ts, dur.
   *
   * Mode is automatically determined:
   * - STATIC mode: when both ts and dur are provided (fixed values)
   * - DYNAMIC mode: when ts or dur is missing (uses trace bounds)
   *
   * In DYNAMIC mode:
   * - If ts is not provided, the backend will use trace_start()
   * - If dur is not provided, the backend will use trace_dur()
   *
   * @param ts The start timestamp in nanoseconds (optional)
   * @param dur The duration in nanoseconds (optional)
   * @param nodeId The node id to assign
   * @returns A new structured query for the time range
   */
  static fromTimeRange(
    ts?: bigint,
    dur?: bigint,
    nodeId?: string,
  ): protos.PerfettoSqlStructuredQuery {
    const sq = new protos.PerfettoSqlStructuredQuery();
    sq.id = nodeId ?? nextNodeId();

    const timeRange =
      new protos.PerfettoSqlStructuredQuery.ExperimentalTimeRange();

    // Determine mode: STATIC if both ts and dur are set, DYNAMIC otherwise
    // Mode enum values: STATIC = 0, DYNAMIC = 1
    const hasTs = ts !== undefined;
    const hasDur = dur !== undefined;
    const isStatic = hasTs && hasDur;
    timeRange.mode = isStatic ? 0 : 1; // 0 = STATIC, 1 = DYNAMIC

    // Convert bigint to number for protobuf (protobufjs uses number for int64)
    if (hasTs) {
      timeRange.ts = Number(ts);
    }
    if (hasDur) {
      timeRange.dur = Number(dur);
    }

    sq.experimentalTimeRange = timeRange;
    return sq;
  }

  /**
   * Creates a structured query with a join operation.
   *
   * @param leftQuery The left query to join (can be a QueryNode or structured query)
   * @param rightQuery The right query to join (can be a QueryNode or structured query)
   * @param joinType The type of join ('INNER', 'LEFT', etc.)
   * @param condition The join condition
   * @param nodeId The node id to assign
   * @returns A new structured query with the join, or undefined if extraction fails
   */
  static withJoin(
    leftQuery: QuerySource,
    rightQuery: QuerySource,
    joinType: 'INNER' | 'LEFT' | 'RIGHT' | 'FULL',
    condition: JoinCondition,
    nodeId?: string,
  ): protos.PerfettoSqlStructuredQuery | undefined {
    const left = extractQuery(leftQuery);
    const right = extractQuery(rightQuery);
    if (!left || !right) return undefined;

    const sq = new protos.PerfettoSqlStructuredQuery();
    sq.id = nodeId ?? nextNodeId();

    const join = new protos.PerfettoSqlStructuredQuery.ExperimentalJoin();
    join.type =
      protos.PerfettoSqlStructuredQuery.ExperimentalJoin.Type[
        joinType as keyof typeof protos.PerfettoSqlStructuredQuery.ExperimentalJoin.Type
      ];
    join.leftQuery = left;
    join.rightQuery = right;

    if (condition.type === 'equality') {
      const equalityCols =
        new protos.PerfettoSqlStructuredQuery.ExperimentalJoin.EqualityColumns();
      equalityCols.leftColumn = condition.leftColumn!;
      equalityCols.rightColumn = condition.rightColumn!;
      join.equalityColumns = equalityCols;
    } else {
      const freeformCond =
        new protos.PerfettoSqlStructuredQuery.ExperimentalJoin.FreeformCondition();
      freeformCond.leftQueryAlias = condition.leftQueryAlias!;
      freeformCond.rightQueryAlias = condition.rightQueryAlias!;
      freeformCond.sqlExpression = condition.sqlExpression!;
      join.freeformCondition = freeformCond;
    }

    sq.experimentalJoin = join;
    return sq;
  }

  /**
   * Creates a structured query with a union operation.
   *
   * @param queries Array of queries to union (can be QueryNodes or structured queries)
   * @param useUnionAll Whether to use UNION ALL instead of UNION
   * @param nodeId The node id to assign
   * @returns A new structured query with the union, or undefined if extraction fails
   */
  static withUnion(
    queries: QuerySource[],
    useUnionAll: boolean = false,
    nodeId?: string,
  ): protos.PerfettoSqlStructuredQuery | undefined {
    const extractedQueries: protos.PerfettoSqlStructuredQuery[] = [];
    for (const q of queries) {
      const query = extractQuery(q);
      if (!query) return undefined;
      extractedQueries.push(query);
    }

    const sq = new protos.PerfettoSqlStructuredQuery();
    sq.id = nodeId ?? nextNodeId();

    const union = new protos.PerfettoSqlStructuredQuery.ExperimentalUnion();
    union.queries = extractedQueries;
    union.useUnionAll = useUnionAll;

    sq.experimentalUnion = union;
    return sq;
  }

  /**
   * Creates a structured query with add columns operation.
   *
   * @param baseQuery The base query (can be a QueryNode or structured query)
   * @param inputQuery The query providing additional columns (can be a QueryNode or structured query)
   * @param inputColumns Columns to add from the input query
   * @param condition Join condition for adding columns
   * @param nodeId The node id to assign
   * @returns A new structured query with added columns, or undefined if extraction fails
   */
  static withAddColumns(
    baseQuery: QuerySource,
    inputQuery: QuerySource,
    inputColumns: ColumnSpec[],
    condition: JoinCondition,
    nodeId?: string,
  ): protos.PerfettoSqlStructuredQuery | undefined {
    const base = extractQuery(baseQuery);
    const input = extractQuery(inputQuery);
    if (!base || !input) return undefined;

    const sq = new protos.PerfettoSqlStructuredQuery();
    sq.id = nodeId ?? nextNodeId();

    const addColumns =
      new protos.PerfettoSqlStructuredQuery.ExperimentalAddColumns();
    addColumns.coreQuery = base;
    addColumns.inputQuery = input;

    addColumns.inputColumns = inputColumns.map((col) => {
      const selectCol = new protos.PerfettoSqlStructuredQuery.SelectColumn();
      selectCol.columnNameOrExpression = col.columnNameOrExpression;
      if (col.alias) {
        selectCol.alias = col.alias;
      }
      return selectCol;
    });

    const equalityCols =
      new protos.PerfettoSqlStructuredQuery.ExperimentalJoin.EqualityColumns();
    equalityCols.leftColumn = condition.leftColumn!;
    equalityCols.rightColumn = condition.rightColumn!;
    addColumns.equalityColumns = equalityCols;

    sq.experimentalAddColumns = addColumns;
    return sq;
  }

  /**
   * Creates a structured query that adds columns from a JOIN and/or computed expressions.
   * This is a higher-level method that handles the complexity of composing
   * JOIN operations with computed columns.
   *
   * @param baseQuery The base query (can be a QueryNode or structured query)
   * @param inputQuery The query providing additional columns via JOIN (optional)
   * @param joinColumns Columns to add from the input query via JOIN (can be empty)
   * @param condition Join condition (required if joinColumns is not empty)
   * @param computedColumns Computed expressions to add as columns (can be empty)
   * @param allBaseColumns All columns from the base query (needed when adding computed columns)
   * @param referencedModules Optional array of referenced module names
   * @param nodeId The node id to assign
   * @returns A new structured query with added columns, or undefined if extraction fails
   */
  static withAddColumnsAndExpressions(
    baseQuery: QuerySource,
    inputQuery: QuerySource | undefined,
    joinColumns: ColumnSpec[],
    condition: JoinCondition | undefined,
    computedColumns: ColumnSpec[],
    allBaseColumns: ColumnSpec[],
    referencedModules?: string[],
    nodeId?: string,
  ): protos.PerfettoSqlStructuredQuery | undefined {
    const hasJoinColumns = joinColumns.length > 0;
    const hasComputedColumns = computedColumns.length > 0;

    // If nothing to add, just return base query
    if (!hasJoinColumns && !hasComputedColumns) {
      return extractQuery(baseQuery);
    }

    let query: protos.PerfettoSqlStructuredQuery | undefined;

    // Step 1: Apply JOIN if we have columns to join
    if (hasJoinColumns && inputQuery && condition) {
      query = this.withAddColumns(
        baseQuery,
        inputQuery,
        joinColumns,
        condition,
        // Use a temporary node ID with '_join' suffix if we'll add computed columns later.
        // This helps with debugging by making intermediate query steps visible.
        hasComputedColumns ? `${nodeId}_join` : nodeId,
      );
    } else {
      query = extractQuery(baseQuery);
    }

    if (!query) return undefined;

    // Step 2: Add computed columns on top if we have any
    if (hasComputedColumns) {
      // Build columns to include: base columns + joined columns (if any) + computed columns
      const allColumns: ColumnSpec[] = [
        ...allBaseColumns,
        // For joined columns, reference them by their alias and preserve the alias in the outer SELECT
        ...joinColumns.map((col) => ({
          columnNameOrExpression: col.alias ?? col.columnNameOrExpression,
          alias: col.alias,
        })),
        ...computedColumns,
      ];

      // Create a temporary node wrapper for the query
      const tempNode: QueryNode = {
        getStructuredQuery: () => query,
      } as QueryNode;

      query = this.withSelectColumns(
        tempNode,
        allColumns,
        referencedModules,
        nodeId,
      );
    }

    return query;
  }

  /**
   * Creates a structured query with filters applied.
   * Wraps the inner query and adds the filter group.
   *
   * @param innerQuery The query to filter (can be a QueryNode or structured query)
   * @param filterGroup The filter group to apply
   * @param nodeId The node id to assign
   * @returns A new structured query with filters, or undefined if extraction fails
   */
  static withFilter(
    innerQuery: QuerySource,
    filterGroup: protos.PerfettoSqlStructuredQuery.ExperimentalFilterGroup,
    nodeId?: string,
  ): protos.PerfettoSqlStructuredQuery | undefined {
    const query = extractQuery(innerQuery);
    if (!query) return undefined;

    const sq = new protos.PerfettoSqlStructuredQuery();
    sq.id = nodeId ?? nextNodeId();
    sq.innerQuery = query;
    sq.experimentalFilterGroup = filterGroup;

    return sq;
  }
}
