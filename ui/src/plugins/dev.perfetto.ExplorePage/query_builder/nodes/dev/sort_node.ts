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
import {
  QueryNode,
  QueryNodeState,
  nextNodeId,
  NodeType,
  ModificationNode,
} from '../../../query_node';
import {ColumnInfo} from '../../column_info';
import protos from '../../../../../protos';
import {Card} from '../../../../../widgets/card';
import {MultiselectInput} from '../../../../../widgets/multiselect_input';

export interface SortNodeState extends QueryNodeState {
  prevNode: QueryNode;
  sortColNames?: string[];
}

export class SortNode implements ModificationNode {
  readonly nodeId: string;
  readonly type = NodeType.kSort;
  readonly prevNode: QueryNode;
  nextNodes: QueryNode[];
  readonly state: SortNodeState;
  sortCols: ColumnInfo[];

  constructor(state: SortNodeState) {
    this.nodeId = nextNodeId();
    this.state = state;
    this.prevNode = state.prevNode;
    this.nextNodes = [];
    this.state.sortColNames = this.state.sortColNames ?? [];
    this.sortCols = this.resolveSortCols();
  }

  private resolveSortCols(): ColumnInfo[] {
    if (!this.state.sortColNames) {
      return [];
    }
    const sourceCols = this.sourceCols;
    return this.state.sortColNames
      .map((name) => sourceCols.find((c) => c.name === name))
      .filter((c): c is ColumnInfo => c !== undefined);
  }

  get sourceCols(): ColumnInfo[] {
    return this.prevNode?.finalCols ?? [];
  }

  get finalCols(): ColumnInfo[] {
    return this.sourceCols;
  }

  getTitle(): string {
    return 'Sort';
  }

  nodeDetails(): m.Child {
    if (this.sortCols.length > 0) {
      const criteria = this.sortCols.map((c) => c.column.name).join(', ');
      return m(
        '.pf-aggregation-node-details',
        `Sort by `,
        m('strong', criteria),
      );
    }
    return m('.pf-aggregation-node-details', 'No sort column selected');
  }

  nodeSpecificModify(): m.Child {
    return m(Card, [
      m('label', 'Pick order by columns '),
      m(MultiselectInput, {
        options: this.sourceCols.map((c) => ({key: c.name, label: c.name})),
        selectedOptions: this.sortCols?.map((c) => c.column.name) ?? [],
        onOptionAdd: (key: string) => {
          if (!this.state.sortColNames) {
            this.state.sortColNames = [];
          }
          this.state.sortColNames.push(key);
          this.sortCols = this.resolveSortCols();
          m.redraw();
        },
        onOptionRemove: (key: string) => {
          if (this.state.sortColNames) {
            this.state.sortColNames = this.state.sortColNames.filter(
              (c) => c !== key,
            );
            this.sortCols = this.resolveSortCols();
            m.redraw();
          }
        },
      }),
      this.sortCols?.map((criterion, index) =>
        m(
          '.sort-criterion',
          {
            draggable: true,
            ondragstart: (e: DragEvent) => {
              e.dataTransfer!.setData('text/plain', index.toString());
            },
            ondragover: (e: DragEvent) => {
              e.preventDefault();
            },
            ondrop: (e: DragEvent) => {
              e.preventDefault();
              if (!this.state.sortColNames) return;
              const from = parseInt(e.dataTransfer!.getData('text/plain'), 10);
              const to = index;

              const newSortCriteria = [...this.state.sortColNames];
              const [removed] = newSortCriteria.splice(from, 1);
              newSortCriteria.splice(to, 0, removed);
              this.state.sortColNames = newSortCriteria;
              this.sortCols = this.resolveSortCols();
              m.redraw();
            },
          },
          [m('span.pf-drag-handle', '☰'), m('span', criterion.column.name)],
        ),
      ),
    ]);
  }

  validate(): boolean {
    return (
      this.prevNode !== undefined &&
      this.sortCols !== undefined &&
      this.sortCols.length > 0
    );
  }

  clone(): QueryNode {
    return new SortNode(this.state);
  }

  getStructuredQuery(): protos.PerfettoSqlStructuredQuery | undefined {
    // TODO(mayzner): Implement this.
    return this.prevNode?.getStructuredQuery();
  }

  serializeState(): object {
    return this.state;
  }

  static deserializeState(state: SortNodeState): SortNodeState {
    return {
      ...state,
      prevNode: undefined as unknown as QueryNode,
    };
  }
}
