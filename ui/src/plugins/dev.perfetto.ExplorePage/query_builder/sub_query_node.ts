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

import m from 'mithril';
import protos from '../../../protos';
import {
  createFinalColumns,
  createSelectColumnsProto,
  nextNodeId,
  NodeType,
  QueryNode,
  QueryNodeState,
} from '../query_node';
import {
  createFiltersProto,
  createGroupByProto,
} from './operations/operation_component';
import {ColumnInfo, newColumnInfoList} from './column_info';
import {assertExists} from '../../../base/logging';

export class SubQueryNode implements QueryNode {
  readonly nodeId: string;
  readonly type = NodeType.kSubQuery;
  readonly prevNode?: QueryNode;
  readonly nextNode?: QueryNode;
  readonly sourceCols: ColumnInfo[];
  readonly finalCols: ColumnInfo[];
  readonly state: QueryNodeState;

  constructor(state: QueryNodeState) {
    assertExists(state.prevNode, 'SubQueryNode requires a previous node');

    this.nodeId = nextNodeId();
    this.state = state;
    this.prevNode = state.prevNode;
    this.sourceCols = this.prevNode!.finalCols;
    this.finalCols = createFinalColumns(this);
  }

  validate(): boolean {
    return this.prevNode !== undefined;
  }

  getTitle(): string {
    return 'Sub-query';
  }

  nodeSpecificModify(): m.Child {
    return undefined;
  }

  clone(): QueryNode {
    const stateCopy: QueryNodeState = {
      prevNode: this.state.prevNode,
      sourceCols: newColumnInfoList(this.sourceCols),
      groupByColumns: newColumnInfoList(this.state.groupByColumns),
      filters: this.state.filters.map((f) => ({...f})),
      aggregations: this.state.aggregations.map((a) => ({...a})),
      customTitle: this.state.customTitle,
      onchange: this.state.onchange,
    };
    return new SubQueryNode(stateCopy);
  }

  getStructuredQuery(): protos.PerfettoSqlStructuredQuery | undefined {
    if (!this.validate()) return;

    const sq = new protos.PerfettoSqlStructuredQuery();
    sq.id = this.nodeId;
    sq.innerQuery = this.prevNode?.getStructuredQuery();

    const filtersProto = createFiltersProto(
      this.state.filters,
      this.sourceCols,
    );
    if (filtersProto) sq.filters = filtersProto;
    const groupByProto = createGroupByProto(
      this.state.groupByColumns,
      this.state.aggregations,
    );
    if (groupByProto !== undefined) sq.groupBy = groupByProto;

    const selectedColumns = createSelectColumnsProto(this);
    if (selectedColumns) sq.selectColumns = selectedColumns;
    return sq;
  }
}
