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

import {
  QueryNode,
  QueryNodeState,
  nextNodeId,
  NodeType,
  ModificationNode,
} from '../../../query_node';
import {ColumnInfo, columnInfoFromName} from '../../column_info';
import protos from '../../../../../protos';
import m from 'mithril';
import {Card} from '../../../../../widgets/card';
import {FilterOperation, UIFilter} from '../../operations/filter';
import {MultiselectInput} from '../../../../../widgets/multiselect_input';

export interface AddColumnsNodeState extends QueryNodeState {
  prevNode: QueryNode;
  selectedColumns?: string[];
}

export class AddColumnsNode implements ModificationNode {
  readonly nodeId: string;
  readonly type = NodeType.kAddColumns;
  readonly prevNode: QueryNode;
  nextNodes: QueryNode[];
  readonly state: AddColumnsNodeState;

  constructor(state: AddColumnsNodeState) {
    this.nodeId = nextNodeId();
    this.state = state;
    this.prevNode = state.prevNode;
    this.nextNodes = [];
    this.state.filters = this.state.filters ?? [];
    this.state.selectedColumns = this.state.selectedColumns ?? [];
  }

  get sourceCols(): ColumnInfo[] {
    return this.prevNode.finalCols ?? [];
  }

  get finalCols(): ColumnInfo[] {
    if (this.state.sqlTable) {
      const newCols =
        this.state.selectedColumns?.map((c) => columnInfoFromName(c)) ?? [];
      return [...this.sourceCols, ...newCols];
    }
    return this.sourceCols;
  }

  getTitle(): string {
    return 'Add Columns';
  }

  nodeDetails(): m.Child {
    if (this.state.sqlTable) {
      if (this.state.selectedColumns && this.state.selectedColumns.length > 0) {
        const plural = this.state.selectedColumns.length > 1 ? 's' : '';
        return m(
          '.pf-aggregation-node-details',
          `Add column${plural} `,
          m('strong', this.state.selectedColumns.join(', ')),
          ' from ',
          m('strong', this.state.sqlTable.name),
          ' using ',
          m('strong', 'id'),
        );
      } else {
        return m(
          '.pf-aggregation-node-details',
          `No columns selected from ${this.state.sqlTable.name}`,
        );
      }
    }
    return m('.pf-aggregation-node-details', 'No table selected');
  }

  nodeSpecificModify(): m.Child {
    if (this.state.sqlTable) {
      return m('div', [
        m(Card, [
          m('div', `Table: ${this.state.sqlTable.name}`),
          m(MultiselectInput, {
            options: this.state.sqlTable.columns.map((c) => ({
              key: c.name,
              label: c.name,
            })),
            selectedOptions: this.state.selectedColumns ?? [],
            onOptionAdd: (key: string) => {
              if (!this.state.selectedColumns) {
                this.state.selectedColumns = [];
              }
              this.state.selectedColumns.push(key);
              m.redraw();
            },
            onOptionRemove: (key: string) => {
              if (this.state.selectedColumns) {
                this.state.selectedColumns = this.state.selectedColumns.filter(
                  (c) => c !== key,
                );
                m.redraw();
              }
            },
          }),
        ]),
        m(FilterOperation, {
          filters: this.state.filters,
          sourceCols: this.finalCols,
          onFiltersChanged: (newFilters: ReadonlyArray<UIFilter>) => {
            this.state.filters = [...newFilters];
          },
        }),
      ]);
    }
    return m('div', 'No table selected');
  }

  validate(): boolean {
    return this.prevNode !== undefined;
  }

  clone(): QueryNode {
    return new AddColumnsNode(this.state);
  }

  getStructuredQuery(): protos.PerfettoSqlStructuredQuery | undefined {
    return this.prevNode.getStructuredQuery();
  }

  serializeState(): object {
    return this.state;
  }

  static deserializeState(
    serializedState: AddColumnsNodeState,
  ): AddColumnsNodeState {
    return {
      ...serializedState,
      prevNode: undefined as unknown as QueryNode,
    };
  }
}
