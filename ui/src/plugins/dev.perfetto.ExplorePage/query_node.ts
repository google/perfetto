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
import {ColumnInfo, newColumnInfoList} from './query_builder/column_info';
import {FilterDefinition} from '../../components/widgets/data_grid/common';
import {Engine} from '../../trace_processor/engine';
import {NodeIssues} from './query_builder/node_issues';

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
  kSubQuery,
  kAggregation,
  kModifyColumns,

  // Multi node operations
  kIntervalIntersect,
}

// All information required to create a new node.
export interface QueryNodeState {
  prevNodes?: QueryNode[];
  customTitle?: string;

  // Operations
  filters: FilterDefinition[];

  issues?: NodeIssues;

  onchange?: () => void;

  // Caching
  isExecuted?: boolean;
  hasOperationChanged?: boolean;
}

export interface QueryNode {
  readonly nodeId: string;
  meterialisedAs?: string;
  readonly type: NodeType;
  prevNodes?: QueryNode[];
  nextNodes: QueryNode[];

  // Columns that are available in the source data.
  readonly sourceCols: ColumnInfo[];

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
  isMaterialised(): boolean;
  serializeState(): object;
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

export function createFinalColumns(node: QueryNode) {
  return newColumnInfoList(node.sourceCols, true);
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
    if (curNode.prevNodes?.[0]) {
      if (!curNode.prevNodes[0].validate()) {
        return;
      }
      curNode = curNode.prevNodes[0];
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
  if (
    node.state.isExecuted &&
    !node.state.hasOperationChanged &&
    node.type !== NodeType.kSqlSource
  ) {
    const sql: Query = {
      sql: `SELECT * FROM ${node.meterialisedAs ?? ''}`,
      textproto: '',
      modules: [],
      preambles: [],
      columns: [],
    };
    return sql;
  }

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

  let finalSql = lastRes.sql;
  if (materialise(node)) {
    if (!node.meterialisedAs) {
      node.meterialisedAs = `exp_${node.nodeId}`;
    }
    const createTableSql = `CREATE OR REPLACE PERFETTO TABLE ${
      node.meterialisedAs ?? `exp_${node.nodeId}`
    } AS \n${lastRes.sql}`;
    const selectSql = `SELECT * FROM ${node.meterialisedAs ?? `exp_${node.nodeId}`}`;
    finalSql = `${createTableSql};\n${selectSql}`;
  }

  const sql: Query = {
    sql: finalSql,
    textproto: lastRes.textproto ?? '',
    modules: lastRes.modules ?? [],
    preambles: lastRes.preambles ?? [],
    columns: lastRes.columns ?? [],
  };
  return sql;
}

export function setOperationChanged(node: QueryNode) {
  let curr: QueryNode | undefined = node;
  while (curr) {
    if (curr.state.hasOperationChanged) {
      // Already marked as changed, and so are the children.
      break;
    }
    curr.state.hasOperationChanged = true;
    const queue: QueryNode[] = [];
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

function materialise(node: QueryNode): boolean {
  return (
    node.type !== NodeType.kSqlSource &&
    node.type != NodeType.kIntervalIntersect
  );
}
