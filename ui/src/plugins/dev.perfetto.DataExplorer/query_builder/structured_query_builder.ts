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
 * ARCHITECTURE: Query Building Strategy - Reference vs. Embedding
 *
 * This builder uses two strategies for composing queries:
 *
 * 1. **Reference by ID (innerQueryId)**: Used for single-input operations
 *    - Operations: SELECT, WHERE, ORDER BY, LIMIT/OFFSET, GROUP BY, etc.
 *    - Advantage: More efficient, creates flatter query graphs
 *    - Implementation: Set innerQueryId to reference the input query's id
 *
 * 2. **Full Query Embedding (innerQuery)**: Used for multi-input operations
 *    - Operations: JOIN, UNION, interval operations
 *    - Reason: These operations need to embed complete query structures
 *    - Implementation: Create intermediate reference queries that wrap the IDs
 *
 * Example of reference-based building:
 *   FilterNode -> SortNode -> LimitNode
 *   Each node references its input by ID, creating a chain of references.
 *
 * Example of embedding (JOIN):
 *   LEFT query + RIGHT query -> JOIN operation
 *   Creates intermediate refs for left/right, then embeds them in the join.
 *
 * This hybrid approach balances efficiency (references) with compatibility
 * (embedding where the proto structure requires full query objects).
 */

/**
 * Type representing a query source.
 * Builder methods accept nodes or structured queries and extract/use them internally.
 */
export type QuerySource =
  | QueryNode
  | protos.PerfettoSqlStructuredQuery
  | undefined;

/**
 * Helper function to extract a structured query from a QuerySource.
 * Used for operations that need to embed the full query (e.g., joins, unions).
 * @param source The query source (node or structured query)
 * @returns The structured query, or undefined if extraction fails
 */
function extractQuery(
  source: QuerySource,
): protos.PerfettoSqlStructuredQuery | undefined {
  if (source === undefined) return undefined;
  if (source instanceof protos.PerfettoSqlStructuredQuery) {
    return source;
  }
  return source.getStructuredQuery();
}

/**
 * Helper function to extract the query ID from a QuerySource.
 * Used for operations that reference queries by ID (single-input operations).
 * @param source The query source (node or structured query)
 * @returns The query ID, or undefined if extraction fails
 */
function extractQueryId(source: QuerySource): string | undefined {
  if (source === undefined) return undefined;
  if (source instanceof protos.PerfettoSqlStructuredQuery) {
    return source.id ?? undefined;
  }
  // For QueryNode, use the node's ID directly. This works because when a node
  // builds its structured query via getStructuredQuery(), it sets the query's
  // id field to the node's nodeId. This allows us to reference the query by
  // the node's ID without needing to call getStructuredQuery() first, which
  // is more efficient for single-input operations that only need the reference.
  return source.nodeId;
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
   * Creates a new structured query with innerQuery wrapper (embedding).
   * Use this for multi-input operations that need full query embedding.
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
   * Creates a passthrough query that references an inner query by ID.
   * Use this when a node has no operation but still needs to maintain
   * the reference chain (e.g., FilterNode with no filters).
   *
   * @param innerQuery The query source to reference
   * @param nodeId The node id to assign
   * @returns A new structured query referencing the inner query by ID
   */
  static passthrough(
    innerQuery: QuerySource,
    nodeId: string,
  ): protos.PerfettoSqlStructuredQuery | undefined {
    const queryId = extractQueryId(innerQuery);
    if (!queryId) return undefined;

    const sq = new protos.PerfettoSqlStructuredQuery();
    sq.id = nodeId;
    sq.innerQueryId = queryId;
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
   * References the inner query by ID (not embedded).
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
    const queryId = extractQueryId(innerQuery);
    if (!queryId) return undefined;

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
      innerQueryId: queryId,
      orderBy: protos.PerfettoSqlStructuredQuery.OrderBy.create({
        orderingSpecs,
      }),
    });
  }

  /**
   * Creates a structured query with LIMIT and/or OFFSET.
   * References the inner query by ID (not embedded).
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
    const queryId = extractQueryId(innerQuery);
    if (!queryId) return undefined;

    const sq = new protos.PerfettoSqlStructuredQuery();
    sq.id = nodeId ?? nextNodeId();
    sq.innerQueryId = queryId;

    if (limit !== undefined && limit >= 0) {
      sq.limit = limit;
    }

    if (offset !== undefined && offset > 0) {
      sq.offset = offset;
    }

    return sq;
  }

  /**
   * Wraps a query with ExperimentalCounterIntervals to convert counter data to intervals.
   * References the input query by ID (not embedded).
   *
   * @param inputQuery The query containing counter data (id, ts, track_id, value)
   * @param nodeId Optional node id. If not provided, generates a new one.
   * @returns A new structured query with counter intervals conversion
   */
  static withCounterIntervals(
    inputQuery: QuerySource,
    nodeId?: string,
  ): protos.PerfettoSqlStructuredQuery | undefined {
    const queryId = extractQueryId(inputQuery);
    if (!queryId) return undefined;

    const actualNodeId = nodeId ?? nextNodeId();

    // Create an intermediate reference query for the input. This is necessary
    // because experimentalCounterIntervals.inputQuery expects a full query object
    // (not just an ID), so we wrap the reference in a passthrough query.
    const inputRef = new protos.PerfettoSqlStructuredQuery();
    inputRef.id = `${actualNodeId}_input_ref`;
    inputRef.innerQueryId = queryId;

    const sq = new protos.PerfettoSqlStructuredQuery();
    sq.id = actualNodeId;
    sq.experimentalCounterIntervals =
      new protos.PerfettoSqlStructuredQuery.ExperimentalCounterIntervals();
    sq.experimentalCounterIntervals.inputQuery = inputRef;
    return sq;
  }

  /**
   * Creates a structured query with interval intersect operation.
   * Automatically filters out unfinished slices (dur < 0) from all inputs.
   * References the input queries by ID (not embedded).
   *
   * @param baseQuery The base query for the intersection
   * @param intervalQueries Array of interval queries to intersect with the base
   * @param partitionColumns Optional partition columns for the intersection
   * @param nodeId The node id to assign
   * @returns A new structured query with interval intersect, or undefined if extraction fails
   */
  static withIntervalIntersect(
    baseQuery: QuerySource,
    intervalQueries: QuerySource[],
    partitionColumns?: string[],
    nodeId?: string,
  ): protos.PerfettoSqlStructuredQuery | undefined {
    const actualNodeId = nodeId ?? nextNodeId();

    // Create reference query for base with dur filter
    const baseId = extractQueryId(baseQuery);
    if (!baseId) return undefined;
    const base = this.createDurFilteredRef(baseId, `${actualNodeId}_base_ref`);

    // Create reference queries for intervals with dur filter
    const intervals: protos.PerfettoSqlStructuredQuery[] = [];
    for (let i = 0; i < intervalQueries.length; i++) {
      const intervalId = extractQueryId(intervalQueries[i]);
      if (!intervalId) return undefined;
      intervals.push(
        this.createDurFilteredRef(intervalId, `${actualNodeId}_interval_${i}`),
      );
    }

    const sq = new protos.PerfettoSqlStructuredQuery();
    sq.id = actualNodeId;
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
   * Creates a simple reference query.
   * This is a private helper method used by multi-input operations.
   *
   * @param innerQueryId The ID of the query to reference
   * @param refId The ID for this reference query
   * @returns A new structured query referencing the inner query
   */
  private static createRef(
    innerQueryId: string,
    refId: string,
  ): protos.PerfettoSqlStructuredQuery {
    const sq = new protos.PerfettoSqlStructuredQuery();
    sq.id = refId;
    sq.innerQueryId = innerQueryId;
    return sq;
  }

  /**
   * Creates a reference query with a dur >= 0 filter applied.
   * This is a private helper method used by multi-input operations.
   *
   * @param innerQueryId The ID of the query to reference
   * @param refId The ID for this reference query
   * @returns A new structured query referencing the inner query with dur filter
   */
  private static createDurFilteredRef(
    innerQueryId: string,
    refId: string,
  ): protos.PerfettoSqlStructuredQuery {
    const sq = new protos.PerfettoSqlStructuredQuery();
    sq.id = refId;
    sq.innerQueryId = innerQueryId;

    // Create the dur >= 0 filter
    const filter = new protos.PerfettoSqlStructuredQuery.Filter();
    filter.columnName = 'dur';
    filter.op =
      protos.PerfettoSqlStructuredQuery.Filter.Operator.GREATER_THAN_EQUAL;
    filter.int64Rhs = [0];
    sq.filters = [filter];

    return sq;
  }

  /**
   * Creates a structured query with GROUP BY and aggregations.
   * References the inner query by ID (not embedded).
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
    const queryId = extractQueryId(innerQuery);
    if (!queryId) return undefined;

    const sq = new protos.PerfettoSqlStructuredQuery();
    sq.id = nodeId;
    sq.innerQueryId = queryId;

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

    sq.groupBy = groupByProto;
    return sq;
  }

  /**
   * Creates a structured query with column selection.
   * References the inner query by ID (not embedded).
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
    const queryId = extractQueryId(innerQuery);
    if (!queryId) return undefined;

    const sq = new protos.PerfettoSqlStructuredQuery();
    sq.id = nodeId ?? nextNodeId();
    sq.innerQueryId = queryId;

    sq.selectColumns = columns.map((col) => {
      const selectCol = new protos.PerfettoSqlStructuredQuery.SelectColumn();
      selectCol.columnNameOrExpression = col.columnNameOrExpression;
      if (col.alias && col.alias.trim() !== '') {
        selectCol.alias = col.alias;
      }
      return selectCol;
    });

    if (referencedModules && referencedModules.length > 0) {
      sq.referencedModules = referencedModules;
    }

    return sq;
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
   * References the input queries by ID (not embedded).
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
    const leftId = extractQueryId(leftQuery);
    const rightId = extractQueryId(rightQuery);
    if (!leftId || !rightId) return undefined;

    const actualNodeId = nodeId ?? nextNodeId();

    // Create reference queries for left and right
    const left = this.createRef(leftId, `${actualNodeId}_left_ref`);
    const right = this.createRef(rightId, `${actualNodeId}_right_ref`);

    const sq = new protos.PerfettoSqlStructuredQuery();
    sq.id = actualNodeId;

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
   * References the input queries by ID (not embedded).
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
    const actualNodeId = nodeId ?? nextNodeId();

    const refQueries: protos.PerfettoSqlStructuredQuery[] = [];
    for (let i = 0; i < queries.length; i++) {
      const queryId = extractQueryId(queries[i]);
      if (!queryId) return undefined;
      refQueries.push(this.createRef(queryId, `${actualNodeId}_union_${i}`));
    }

    const sq = new protos.PerfettoSqlStructuredQuery();
    sq.id = actualNodeId;

    const union = new protos.PerfettoSqlStructuredQuery.ExperimentalUnion();
    union.queries = refQueries;
    union.useUnionAll = useUnionAll;

    sq.experimentalUnion = union;
    return sq;
  }

  /**
   * Creates a structured query with add columns operation.
   * References the input queries by ID (not embedded).
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
    const baseId = extractQueryId(baseQuery);
    const inputId = extractQueryId(inputQuery);
    if (!baseId || !inputId) return undefined;

    const actualNodeId = nodeId ?? nextNodeId();

    // Create reference queries for base and input
    const base = this.createRef(baseId, `${actualNodeId}_base_ref`);
    const input = this.createRef(inputId, `${actualNodeId}_input_ref`);

    const sq = new protos.PerfettoSqlStructuredQuery();
    sq.id = actualNodeId;

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
        // If we'll add computed columns, this is an intermediate query that will be
        // embedded (not referenced by ID), so the ID doesn't matter.
        hasComputedColumns ? nextNodeId() : nodeId,
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

      // Build SELECT query that EMBEDS the intermediate query (not references by ID).
      // This is necessary because the intermediate query isn't in the queries array -
      // it's only returned as part of this composite operation.
      const sq = new protos.PerfettoSqlStructuredQuery();
      sq.id = nodeId ?? nextNodeId();
      sq.innerQuery = query; // Embed, don't reference by ID

      sq.selectColumns = allColumns.map((col) => {
        const selectCol = new protos.PerfettoSqlStructuredQuery.SelectColumn();
        selectCol.columnNameOrExpression = col.columnNameOrExpression;
        if (col.alias && col.alias.trim() !== '') {
          selectCol.alias = col.alias;
        }
        return selectCol;
      });

      if (referencedModules && referencedModules.length > 0) {
        sq.referencedModules = referencedModules;
      }

      query = sq;
    }

    return query;
  }

  /**
   * Creates a structured query with filters applied.
   * References the inner query by ID (not embedded).
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
    const queryId = extractQueryId(innerQuery);
    if (!queryId) return undefined;

    const sq = new protos.PerfettoSqlStructuredQuery();
    sq.id = nodeId ?? nextNodeId();
    sq.innerQueryId = queryId;
    sq.experimentalFilterGroup = filterGroup;

    return sq;
  }

  /**
   * Creates a structured query with filter-to-intervals operation.
   * Automatically filters out unfinished slices (dur < 0) from both inputs.
   * Filters the base query to only include rows that overlap with intervals
   * from the intervals query. The output preserves the base query's schema.
   * References the input queries by ID (not embedded).
   *
   * Key features:
   * - Overlapping intervals in the filter set are automatically merged
   * - Output schema matches the base query exactly
   * - Supports optional clipping of ts/dur to interval boundaries
   *
   * @param baseQuery The base query containing intervals to filter
   * @param intervalsQuery The query containing the time intervals to filter to
   * @param partitionColumns Optional partition columns for the filtering
   * @param clipToIntervals Whether to clip ts/dur to interval boundaries (default: true)
   * @param nodeId The node id to assign
   * @returns A new structured query with filter-to-intervals, or undefined if extraction fails
   */
  static withFilterToIntervals(
    baseQuery: QuerySource,
    intervalsQuery: QuerySource,
    partitionColumns?: string[],
    clipToIntervals?: boolean,
    nodeId?: string,
    selectColumns?: string[],
  ): protos.PerfettoSqlStructuredQuery | undefined {
    const actualNodeId = nodeId ?? nextNodeId();

    // Create reference queries with dur filter
    const baseId = extractQueryId(baseQuery);
    if (!baseId) return undefined;
    const base = this.createDurFilteredRef(baseId, `${actualNodeId}_base_ref`);

    const intervalsId = extractQueryId(intervalsQuery);
    if (!intervalsId) return undefined;
    const intervals = this.createDurFilteredRef(
      intervalsId,
      `${actualNodeId}_intervals_ref`,
    );

    const sq = new protos.PerfettoSqlStructuredQuery();
    sq.id = actualNodeId;

    const filterToIntervals =
      new protos.PerfettoSqlStructuredQuery.ExperimentalFilterToIntervals();
    filterToIntervals.base = base;
    filterToIntervals.intervals = intervals;

    if (partitionColumns && partitionColumns.length > 0) {
      filterToIntervals.partitionColumns = [...partitionColumns];
    }

    // clip_to_intervals defaults to true in the proto, so only set if false
    if (clipToIntervals === false) {
      filterToIntervals.clipToIntervals = false;
    }

    if (selectColumns && selectColumns.length > 0) {
      filterToIntervals.selectColumns = [...selectColumns];
    }

    sq.experimentalFilterToIntervals = filterToIntervals;
    return sq;
  }

  /**
   * Creates a structured query with filter-in operation (semi-join).
   * Filters rows from the base query where a column's values exist in
   * another column from the match values query.
   * References the input queries by ID (not embedded).
   *
   * @param baseQuery The base query containing rows to filter
   * @param matchValuesQuery The query containing values to match against
   * @param baseColumn The column name in the base query to filter on
   * @param matchColumn The column name in the match_values query to match against
   * @param nodeId The node id to assign
   * @returns A new structured query with filter-in, or undefined if extraction fails
   */
  static withFilterIn(
    baseQuery: QuerySource,
    matchValuesQuery: QuerySource,
    baseColumn: string,
    matchColumn: string,
    nodeId?: string,
  ): protos.PerfettoSqlStructuredQuery | undefined {
    const actualNodeId = nodeId ?? nextNodeId();

    // Create reference queries
    const baseId = extractQueryId(baseQuery);
    if (!baseId) return undefined;
    const base = this.createRef(baseId, `${actualNodeId}_base_ref`);

    const matchValuesId = extractQueryId(matchValuesQuery);
    if (!matchValuesId) return undefined;
    const matchValues = this.createRef(
      matchValuesId,
      `${actualNodeId}_match_values_ref`,
    );

    const sq = new protos.PerfettoSqlStructuredQuery();
    sq.id = actualNodeId;

    const filterIn =
      new protos.PerfettoSqlStructuredQuery.ExperimentalFilterIn();
    filterIn.base = base;
    filterIn.matchValues = matchValues;
    filterIn.baseColumn = baseColumn;
    filterIn.matchColumn = matchColumn;

    sq.experimentalFilterIn = filterIn;
    return sq;
  }
}
