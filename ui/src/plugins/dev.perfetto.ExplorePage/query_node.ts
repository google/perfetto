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
import {
  ColumnControllerRow,
  columnControllerRowFromName,
  newColumnControllerRows,
} from './query_builder/column_controller';
import {
  GroupByAgg,
  placeholderNewColumnName,
} from './query_builder/operations/groupy_by';
import {Filter} from './query_builder/operations/filter';

export enum NodeType {
  // Sources
  kStdlibTable,
  kSimpleSlices,
  kSqlSource,

  // Operations
  kJoinOperator,
  kGroupByOperator,
  kFilterOperator,
}

// All information required to create a new node.
export interface QueryNodeState {
  prevNode?: QueryNode;
  sourceCols: ColumnControllerRow[];

  filters: Filter[];
  groupByColumns: ColumnControllerRow[];
  aggregations: GroupByAgg[];
}

export interface QueryNode {
  readonly type: NodeType;
  readonly prevNode?: QueryNode;
  nextNode?: QueryNode;

  readonly dataName: string;
  readonly sourceCols: ColumnControllerRow[];
  readonly finalCols: ColumnControllerRow[];

  readonly attrs: QueryNodeState;

  validate(): boolean;
  getTitle(): string;
  getDetails(): m.Child;
  getAttrs(): QueryNodeState;
  getStructuredQuery(): protos.PerfettoSqlStructuredQuery | undefined;
}

export function getLastFinishedNode(node: QueryNode): QueryNode | undefined {
  while (node.nextNode) {
    node = node.nextNode;
  }
  return node;
}

export function getFirstNode(node: QueryNode): QueryNode | undefined {
  while (node.prevNode) {
    node = node.prevNode;
  }
  return node;
}

export function createSelectColumnsProto(
  node: QueryNode,
): protos.PerfettoSqlStructuredQuery.SelectColumn[] | undefined {
  if (node.finalCols?.every((c) => c.checked)) return;
  const selectedColumns: protos.PerfettoSqlStructuredQuery.SelectColumn[] = [];

  for (const c of node.finalCols ?? []) {
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
  if (node.attrs.groupByColumns.find((c) => c.checked)) {
    const selected = node.attrs.groupByColumns.filter((c) => c.checked);
    for (const agg of node.attrs.aggregations) {
      selected.push(
        columnControllerRowFromName(
          agg.newColumnName ?? placeholderNewColumnName(agg),
        ),
      );
    }
    return newColumnControllerRows(selected, true);
  }

  return newColumnControllerRows(node.sourceCols, true);
}
