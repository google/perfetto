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

  // Multi node operations
  kIntervalIntersect,
  kUnion,
}

export function singleNodeOperation(type: NodeType): boolean {
  switch (type) {
    case NodeType.kAggregation:
    case NodeType.kModifyColumns:
    case NodeType.kAddColumns:
    case NodeType.kLimitAndOffset:
    case NodeType.kSort:
      return true;
    default:
      return false;
  }
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

  issues?: NodeIssues;

  onchange?: () => void;

  // Caching
  hasOperationChanged?: boolean;

  // Whether queries should automatically execute when this node changes.
  // If false, the user must manually click "Run" to execute queries.
  // Set by the node registry when the node is created.
  autoExecute?: boolean;
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
  nodeSpecificModify(onExecute?: () => void): m.Child;
  nodeDetails?(): m.Child | undefined;
  clone(): QueryNode;
  getStructuredQuery(): protos.PerfettoSqlStructuredQuery | undefined;
  serializeState(): object;
  onPrevNodesUpdated?(): void;
}

export interface SourceNode extends BaseNode {}

export interface ModificationNode extends BaseNode {
  prevNode?: QueryNode;
}

export interface MultiSourceNode extends BaseNode {
  prevNodes: (QueryNode | undefined)[];
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
  return includes.join('\n') + query.preambles.join('\n') + query.sql;
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
