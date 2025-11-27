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

let nodeCounter = 0;
export function nextNodeId(): string {
  return (nodeCounter++).toString();
}

export enum NodeType {
  // Sources
  kTable,
  kSimpleSlices,
  kSqlSource,

  // Single node operations
  kAggregation,
  kModifyColumns,
  kAddColumns,
  kLimitAndOffset,
  kSort,
  kFilter,

  // Multi node operations
  kIntervalIntersect,
  kUnion,
  kMerge,
}

export function singleNodeOperation(type: NodeType): boolean {
  switch (type) {
    case NodeType.kAggregation:
    case NodeType.kModifyColumns:
    case NodeType.kAddColumns:
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

// All information required to create a new node.
export interface QueryNodeState {
  prevNode?: QueryNode;
  prevNodes?: QueryNode[];
  comment?: string;
  trace?: Trace;
  sqlModules?: SqlModules;
  sqlTable?: SqlTable;

  // Operations
  filters?: UIFilter[];
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

export interface BaseNode {
  readonly nodeId: string;
  readonly type: NodeType;
  nextNodes: QueryNode[];

  // Columns that are available after applying all operations.
  readonly finalCols: ColumnInfo[];

  // State of the node. This is used to store the user's input and can be used
  // to fully recover the node.
  readonly state: QueryNodeState;

  validate(): boolean;
  getTitle(): string;
  nodeSpecificModify(): m.Child;
  nodeDetails?(): m.Child | undefined;
  nodeInfo(): m.Children;
  clone(): QueryNode;
  getStructuredQuery(): protos.PerfettoSqlStructuredQuery | undefined;
  serializeState(): object;
  onPrevNodesUpdated?(): void;
}

export interface SourceNode extends BaseNode {}

export interface ModificationNode extends BaseNode {
  prevNode?: QueryNode;
  // Optional input nodes that appear on the left side of the node
  // (as opposed to prevNode which comes from above)
  inputNodes?: (QueryNode | undefined)[];
}

export interface MultiSourceNode extends BaseNode {
  prevNodes: QueryNode[];
}

export type QueryNode = SourceNode | ModificationNode | MultiSourceNode;

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

    let prevNode: QueryNode | undefined;
    if ('prevNode' in curNode) {
      prevNode = curNode.prevNode;
    } else if ('prevNodes' in curNode && curNode.prevNodes.length > 0) {
      prevNode = curNode.prevNodes[0];
    }

    if (prevNode) {
      if (!prevNode.validate()) {
        return;
      }
      curNode = prevNode;
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
 */
export function hashNodeQuery(node: QueryNode): string | undefined {
  const sq = node.getStructuredQuery();
  if (sq === undefined) {
    return undefined;
  }

  // JSON.stringify on the protobuf object gives us a stable representation
  // of all the query structure (filters, aggregations, joins, etc.).
  // Protobuf objects have stable field ordering, making this deterministic.
  return JSON.stringify(sq);
}

export async function analyzeNode(
  node: QueryNode,
  engine: Engine,
): Promise<Query | undefined | Error> {
  const structuredQueries = getStructuredQueries(node);
  if (structuredQueries === undefined) return;

  const res = await engine.analyzeStructuredQuery(structuredQueries);
  if (res.error) return Error(res.error);
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
  if (!lastRes.textproto) {
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

export function setOperationChanged(node: QueryNode) {
  let curr: QueryNode | undefined = node;
  const queue: QueryNode[] = [];
  while (curr) {
    if (curr.state.hasOperationChanged) {
      // Already marked as changed, skip this branch
      curr = queue.shift();
      continue;
    }
    curr.state.hasOperationChanged = true;
    curr.nextNodes.forEach((child) => {
      queue.push(child);
    });
    curr = queue.shift();
  }
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

  // Update backward link based on node type
  if ('prevNode' in toNode && singleNodeOperation(toNode.type)) {
    // ModificationNode
    const modNode = toNode as ModificationNode;

    // If portIndex is specified and node supports inputNodes
    if (portIndex !== undefined && 'inputNodes' in modNode) {
      // portIndex maps directly to inputNodes array
      // portIndex=0 → inputNodes[0], portIndex=1 → inputNodes[1], etc.
      if (!modNode.inputNodes) {
        modNode.inputNodes = [];
      }
      // Expand array if needed
      while (modNode.inputNodes.length <= portIndex) {
        modNode.inputNodes.push(undefined);
      }
      modNode.inputNodes[portIndex] = fromNode;
      modNode.onPrevNodesUpdated?.();
    } else {
      // Otherwise connect to prevNode (default single input from above)
      modNode.prevNode = fromNode;
      modNode.onPrevNodesUpdated?.();
    }
  } else if ('prevNodes' in toNode && Array.isArray(toNode.prevNodes)) {
    // MultiSourceNode - multiple inputs
    const multiSourceNode = toNode as MultiSourceNode;

    if (
      portIndex !== undefined &&
      portIndex < multiSourceNode.prevNodes.length
    ) {
      // Replace existing connection at this port
      multiSourceNode.prevNodes[portIndex] = fromNode;
    } else {
      // Append to end (ignore portIndex if out of bounds)
      multiSourceNode.prevNodes.push(fromNode);
    }
    multiSourceNode.onPrevNodesUpdated?.();
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

  // Remove backward link based on node type
  if ('prevNode' in toNode && singleNodeOperation(toNode.type)) {
    // ModificationNode
    const modNode = toNode as ModificationNode;

    // Check if it's in prevNode
    if (modNode.prevNode === fromNode) {
      modNode.prevNode = undefined;
    }

    // Also check if it's in inputNodes
    if ('inputNodes' in modNode && modNode.inputNodes) {
      const inputIndex = modNode.inputNodes.indexOf(fromNode);
      if (inputIndex !== -1) {
        modNode.inputNodes[inputIndex] = undefined;
        modNode.onPrevNodesUpdated?.();
      }
    }
  } else if ('prevNodes' in toNode && Array.isArray(toNode.prevNodes)) {
    // MultiSourceNode - multiple inputs
    const multiSourceNode = toNode as MultiSourceNode;
    const prevIndex = multiSourceNode.prevNodes.indexOf(fromNode);
    if (prevIndex !== -1) {
      // Remove from array, compacting it (no undefined holes)
      multiSourceNode.prevNodes.splice(prevIndex, 1);
      multiSourceNode.onPrevNodesUpdated?.();
    }
  }
}
