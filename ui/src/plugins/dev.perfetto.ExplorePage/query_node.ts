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

import protos from '../../protos';
import m from 'mithril';
import {SqlModules, SqlTable} from '../dev.perfetto.SqlModules/sql_modules';
import {ColumnInfo, newColumnInfoList} from './query_builder/column_info';
import {UIFilter} from './query_builder/operations/filter';
import {Engine} from '../../trace_processor/engine';
import {NodeIssues} from './query_builder/node_issues';
import {Trace} from '../../public/trace';
import {stringifyJsonWithBigints} from '../../base/json_utils';
import {NodeDetailsAttrs} from './query_builder/node_explorer_types';

let nodeCounter = 0;
export function nextNodeId(): string {
  return (nodeCounter++).toString();
}

export enum NodeType {
  // Sources
  kTable,
  kSimpleSlices,
  kSqlSource,
  kTimeRangeSource,

  // Single node operations
  kAggregation,
  kModifyColumns,
  kAddColumns,
  kFilterDuring,
  kLimitAndOffset,
  kSort,
  kFilter,

  // Multi node operations
  kIntervalIntersect,
  kUnion,
  kJoin,
  kCreateSlices,

  // Deprecated (kept for backward compatibility)
  kMerge = kJoin,
}

export function singleNodeOperation(type: NodeType): boolean {
  switch (type) {
    case NodeType.kAggregation:
    case NodeType.kModifyColumns:
    case NodeType.kAddColumns:
    case NodeType.kFilterDuring:
    case NodeType.kLimitAndOffset:
    case NodeType.kSort:
    case NodeType.kFilter:
      return true;
    default:
      return false;
  }
}

// Actions that can be performed by nodes on the parent graph.
// These are optional callbacks provided by the parent component.
export interface NodeActions {
  // Create and connect a table node to a target node's input port
  onAddAndConnectTable?: (tableName: string, portIndex: number) => void;
  // Insert a ModifyColumns node on an input at a specific port
  onInsertModifyColumnsNode?: (portIndex: number) => void;
}

// Specification for secondary inputs with clear cardinality requirements
export interface SecondaryInputSpec {
  // The actual connections (no undefined holes - indexed by port number)
  readonly connections: Map<number, QueryNode>;

  // Cardinality requirements for validation
  readonly min: number; // Minimum required (e.g., 2 for IntervalIntersect)
  readonly max: number | 'unbounded'; // Maximum allowed (e.g., 2 for Join, unbounded for IntervalIntersect)

  // Port names for UI display
  // Can be an array of names or a function that generates a name for a given port index
  readonly portNames: string[] | ((portIndex: number) => string);
}

// All information required to create a new node.
export interface QueryNodeState {
  trace?: Trace;
  sqlModules?: SqlModules;
  sqlTable?: SqlTable;

  // Operations
  // Filters can be partial during editing (similar to how Aggregation works)
  filters?: Partial<UIFilter>[];
  filterOperator?: 'AND' | 'OR'; // How to combine filters (default: AND)

  issues?: NodeIssues;

  onchange?: () => void;

  // Actions that can be performed on the parent graph
  actions?: NodeActions;

  // Caching
  hasOperationChanged?: boolean;

  // Whether queries should automatically execute when this node changes.
  // If false, the user must manually click "Run" to execute queries.
  // Set by the node registry when the node is created.
  autoExecute?: boolean;

  // Materialization state
  materialized?: boolean;
  materializationTableName?: string;
  // Hash of the query that was materialized (for detecting query changes)
  materializedQueryHash?: string;
}

export interface QueryNode {
  readonly nodeId: string;
  readonly type: NodeType;
  nextNodes: QueryNode[];

  // Columns that are available after applying all operations.
  readonly finalCols: ColumnInfo[];

  // State of the node. This is used to store the user's input and can be used
  // to fully recover the node.
  readonly state: QueryNodeState;

  // Primary input from above (data flows vertically down)
  // Used by single-input operations (Filter, Sort, Aggregation, etc.)
  primaryInput?: QueryNode;

  // Secondary inputs from the side (horizontal connections)
  // Used by multi-input operations (Union, Join, IntervalIntersect) and
  // for side joins (AddColumns)
  secondaryInputs?: SecondaryInputSpec;

  validate(): boolean;
  getTitle(): string;
  // Returns either NodeModifyAttrs (new structured pattern) or m.Child (legacy pattern)
  // NodeModifyAttrs allows nodes to declaratively specify sections and corner buttons,
  // while m.Child allows direct rendering for backwards compatibility
  nodeSpecificModify(): unknown;
  nodeDetails(): NodeDetailsAttrs;
  nodeInfo(): m.Children;
  clone(): QueryNode;
  getStructuredQuery(): protos.PerfettoSqlStructuredQuery | undefined;
  serializeState(): object;
  onPrevNodesUpdated?(): void;
}

export function notifyNextNodes(node: QueryNode) {
  for (const nextNode of node.nextNodes) {
    nextNode.onPrevNodesUpdated?.();
  }
}

export interface Query {
  sql: string;
  textproto: string;
  modules: string[];
  preambles: string[];
  columns: string[];
}

export function createSelectColumnsProto(
  node: QueryNode,
): protos.PerfettoSqlStructuredQuery.SelectColumn[] | undefined {
  if (node.finalCols.every((c) => c.checked)) return;
  const selectedColumns: protos.PerfettoSqlStructuredQuery.SelectColumn[] = [];

  for (const c of node.finalCols) {
    if (c.checked === false) continue;
    const newC = new protos.PerfettoSqlStructuredQuery.SelectColumn();
    newC.columnName = c.column.name;
    if (c.alias) {
      newC.alias = c.alias;
    }
    selectedColumns.push(newC);
  }
  return selectedColumns;
}

export function createFinalColumns(sourceCols: ColumnInfo[]) {
  return newColumnInfoList(sourceCols, true);
}

function getStructuredQueries(
  finalNode: QueryNode,
): protos.PerfettoSqlStructuredQuery[] | undefined {
  if (finalNode.finalCols === undefined) {
    return;
  }
  const revStructuredQueries: protos.PerfettoSqlStructuredQuery[] = [];
  let curNode: QueryNode | undefined = finalNode;
  while (curNode) {
    const curSq = curNode.getStructuredQuery();
    if (curSq === undefined) {
      return;
    }
    revStructuredQueries.push(curSq);

    // Navigate up the graph - prefer primaryInput, fall back to first secondary
    let inputNode: QueryNode | undefined = curNode.primaryInput;
    if (!inputNode && curNode.secondaryInputs) {
      // No primary input - follow first secondary input (arbitrary choice for traversal)
      const connections: Map<number, QueryNode> =
        curNode.secondaryInputs.connections;
      if (connections.size > 0) {
        inputNode = connections.get(0);
      }
    }

    if (inputNode) {
      if (!inputNode.validate()) {
        return;
      }
      curNode = inputNode;
    } else {
      curNode = undefined;
    }
  }
  return revStructuredQueries.reverse();
}

export function queryToRun(query?: Query): string {
  if (query === undefined) return 'N/A';
  const includes = query.modules.map((c) => `INCLUDE PERFETTO MODULE ${c};`);
  const parts: string[] = [];

  // Add INCLUDE statements with newlines after each
  if (includes.length > 0) {
    parts.push(includes.join('\n'));
  }

  // Add preambles with newlines after each
  if (query.preambles.length > 0) {
    parts.push(query.preambles.join('\n'));
  }

  // Add an extra empty line before the SQL if there are any includes or preambles
  if (parts.length > 0) {
    parts.push(''); // This creates the empty line
  }

  // Add the SQL
  parts.push(query.sql);

  return parts.join('\n');
}

/**
 * Computes a hash of a node's structured query for comparison.
 * Used to detect if a query has changed and materialization needs to be redone.
 *
 * Uses the structured query protobuf directly - no engine analysis needed.
 * This allows detecting query changes before any SQL execution.
 *
 * This function is relatively expensive (stringifyJsonWithBigints on entire query tree).
 * QueryExecutionService caches results to avoid recomputation.
 */
export function hashNodeQuery(node: QueryNode): string | undefined {
  const sq = node.getStructuredQuery();
  if (sq === undefined) {
    return undefined;
  }

  // stringifyJsonWithBigints on the protobuf object gives us a stable representation
  // of all the query structure (filters, aggregations, joins, etc.).
  // Protobuf objects have stable field ordering, making this deterministic.
  // Uses bigint-safe stringify to handle bigint values correctly.
  return stringifyJsonWithBigints(sq);
}

export async function analyzeNode(
  node: QueryNode,
  engine: Engine,
): Promise<Query | undefined | Error> {
  const structuredQueries = getStructuredQueries(node);
  if (structuredQueries === undefined) return;

  const res = await engine.analyzeStructuredQuery(structuredQueries);
  if (res.error !== undefined && res.error !== null && res.error !== '') {
    return Error(res.error);
  }
  if (res.results.length === 0) return Error('No structured query results');
  if (res.results.length !== structuredQueries.length) {
    return Error(
      `Wrong structured query results. Asked for ${
        structuredQueries.length
      }, received ${res.results.length}`,
    );
  }

  const lastRes = res.results[res.results.length - 1];
  if (lastRes.sql === null || lastRes.sql === undefined) {
    return;
  }
  if (
    lastRes.textproto === undefined ||
    lastRes.textproto === null ||
    lastRes.textproto === ''
  ) {
    return Error('No textproto in structured query results');
  }

  const sql: Query = {
    sql: lastRes.sql,
    textproto: lastRes.textproto ?? '',
    modules: lastRes.modules ?? [],
    preambles: lastRes.preambles ?? [],
    columns: lastRes.columns ?? [],
  };
  return sql;
}

/**
 * Marks a node's operation as changed.
 * This indicates the node needs re-validation and re-execution.
 *
 * Note: Does not propagate to children or invalidate caches.
 * Use QueryExecutionService.invalidateNode() for invalidation with propagation.
 */
export function setOperationChanged(node: QueryNode) {
  node.state.hasOperationChanged = true;
}

export function isAQuery(
  maybeQuery: Query | undefined | Error,
): maybeQuery is Query {
  return (
    maybeQuery !== undefined &&
    !(maybeQuery instanceof Error) &&
    maybeQuery.sql !== undefined
  );
}

// ========================================
// GRAPH CONNECTION OPERATIONS
// ========================================
// These functions encapsulate the bidirectional relationship management
// between nodes, ensuring consistency when adding/removing connections.

/**
 * Helper: Get all input nodes from a node (both primary and secondary)
 */
export function getInputNodes(node: QueryNode): QueryNode[] {
  const inputs: QueryNode[] = [];

  if (node.primaryInput) {
    inputs.push(node.primaryInput);
  }

  if (node.secondaryInputs) {
    for (const inputNode of node.secondaryInputs.connections.values()) {
      inputs.push(inputNode);
    }
  }

  return inputs;
}

/**
 * Helper: Get secondary input at specific port
 */
export function getSecondaryInput(
  node: QueryNode,
  portIndex: number,
): QueryNode | undefined {
  return node.secondaryInputs?.connections.get(portIndex);
}

/**
 * Helper: Set secondary input at specific port
 */
export function setSecondaryInput(
  node: QueryNode,
  portIndex: number,
  inputNode: QueryNode,
): void {
  if (!node.secondaryInputs) {
    throw new Error('Node does not support secondary inputs');
  }
  node.secondaryInputs.connections.set(portIndex, inputNode);
}

/**
 * Helper: Remove secondary input at specific port
 */
export function removeSecondaryInput(node: QueryNode, portIndex: number): void {
  if (!node.secondaryInputs) return;
  node.secondaryInputs.connections.delete(portIndex);
}

/**
 * Validates that secondary inputs meet cardinality requirements.
 * Returns an error message if validation fails, undefined if valid.
 */
export function validateSecondaryInputs(node: QueryNode): string | undefined {
  if (!node.secondaryInputs) {
    return undefined;
  }

  const {connections, min, max} = node.secondaryInputs;
  const count = connections.size;

  if (count < min) {
    return `Requires at least ${min} input${min === 1 ? '' : 's'}, but only ${count} connected`;
  }

  if (max !== 'unbounded' && count > max) {
    return `Allows at most ${max} input${max === 1 ? '' : 's'}, but ${count} connected`;
  }

  return undefined;
}

/**
 * Adds a connection from one node to another, updating both forward and
 * backward links. For multi-source nodes, adds to the specified port index.
 */
export function addConnection(
  fromNode: QueryNode,
  toNode: QueryNode,
  portIndex?: number,
): void {
  // Update forward link (fromNode -> toNode)
  if (!fromNode.nextNodes.includes(toNode)) {
    fromNode.nextNodes.push(toNode);
  }

  // Determine connection type based on node characteristics
  if (singleNodeOperation(toNode.type)) {
    // Single-input operation node (Filter, Sort, etc.)
    // If portIndex is specified, connect to secondary input
    if (portIndex !== undefined) {
      if (!toNode.secondaryInputs) {
        throw new Error(
          `Node ${toNode.nodeId} does not support secondary inputs`,
        );
      }
      setSecondaryInput(toNode, portIndex, fromNode);
    } else {
      // Otherwise connect to primary input (default from above)
      toNode.primaryInput = fromNode;
    }
    toNode.onPrevNodesUpdated?.();
  } else if (toNode.secondaryInputs) {
    // Multi-source node (Union, Join, IntervalIntersect)
    if (portIndex !== undefined) {
      // Set at specific port
      setSecondaryInput(toNode, portIndex, fromNode);
    } else {
      // Find first available port
      let nextPort = 0;
      while (toNode.secondaryInputs.connections.has(nextPort)) {
        nextPort++;
      }
      setSecondaryInput(toNode, nextPort, fromNode);
    }
    toNode.onPrevNodesUpdated?.();
  }
}

/**
 * Removes a connection from one node to another, cleaning up both forward
 * and backward links.
 */
export function removeConnection(fromNode: QueryNode, toNode: QueryNode): void {
  // Remove forward link (fromNode -> toNode)
  const nextIndex = fromNode.nextNodes.indexOf(toNode);
  if (nextIndex !== -1) {
    fromNode.nextNodes.splice(nextIndex, 1);
  }

  // Check if it's in primary input
  if (toNode.primaryInput === fromNode) {
    toNode.primaryInput = undefined;
    toNode.onPrevNodesUpdated?.();
  }

  // Also check if it's in secondary inputs
  if (toNode.secondaryInputs) {
    for (const [portIndex, inputNode] of toNode.secondaryInputs.connections) {
      if (inputNode === fromNode) {
        removeSecondaryInput(toNode, portIndex);
        toNode.onPrevNodesUpdated?.();
        break;
      }
    }
  }
}
