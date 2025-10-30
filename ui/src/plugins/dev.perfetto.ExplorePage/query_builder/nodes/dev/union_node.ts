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
  MultiSourceNode,
  notifyNextNodes,
} from '../../../query_node';
import protos from '../../../../../protos';
import {ColumnInfo, newColumnInfoList} from '../../column_info';
import {Callout} from '../../../../../widgets/callout';
import {NodeIssues} from '../../node_issues';
import {UIFilter} from '../../operations/filter';
import {Card, CardStack} from '../../../../../widgets/card';
import {Checkbox} from '../../../../../widgets/checkbox';

export interface UnionSerializedState {
  unionNodes: string[];
  selectedColumns: ColumnInfo[];
  filters?: UIFilter[];
  comment?: string;
}

export interface UnionNodeState extends QueryNodeState {
  readonly prevNodes: QueryNode[];
  selectedColumns: ColumnInfo[];
  onExecute?: () => void;
}

export class UnionNode implements MultiSourceNode {
  readonly nodeId: string;
  readonly type = NodeType.kUnion;
  readonly prevNodes: QueryNode[];
  nextNodes: QueryNode[];
  readonly state: UnionNodeState;
  comment?: string;
  filters?: UIFilter[];

  get finalCols(): ColumnInfo[] {
    return this.state.selectedColumns.filter((col) => col.checked);
  }

  constructor(state: UnionNodeState) {
    this.nodeId = nextNodeId();
    this.state = {
      ...state,
      autoExecute: state.autoExecute ?? false,
    };
    this.prevNodes = state.prevNodes;
    this.nextNodes = [];

    const userOnChange = this.state.onchange;
    this.state.onchange = () => {
      notifyNextNodes(this);
      userOnChange?.();
    };
  }

  onPrevNodesUpdated() {
    const newCommonColumns = this.getCommonColumns();

    // Preserve checked status for columns that still exist.
    for (const oldCol of this.state.selectedColumns ?? []) {
      const newCol = newCommonColumns.find(
        (c) => c.column.name === oldCol.column.name,
      );
      if (newCol) {
        newCol.checked = oldCol.checked;
      }
    }

    this.state.selectedColumns = newCommonColumns;
  }

  private getCommonColumns(): ColumnInfo[] {
    if (this.prevNodes.length === 0) {
      return [];
    }
    let commonCols = newColumnInfoList(this.prevNodes[0].finalCols, true);
    for (let i = 1; i < this.prevNodes.length; i++) {
      const currentNodeCols = this.prevNodes[i].finalCols;
      commonCols = commonCols.filter((commonCol) =>
        currentNodeCols.some(
          (currentNodeCol) =>
            currentNodeCol.column.name === commonCol.column.name,
        ),
      );
    }
    return commonCols;
  }

  validate(): boolean {
    if (this.prevNodes.length < 2) {
      if (!this.state.issues) this.state.issues = new NodeIssues();
      this.state.issues.queryError = new Error(
        'Union node requires at least two sources.',
      );
      return false;
    }

    if (this.getCommonColumns().length === 0) {
      if (!this.state.issues) this.state.issues = new NodeIssues();
      this.state.issues.queryError = new Error(
        'Union node requires common columns between sources.',
      );
      return false;
    }

    // If the basic structure is valid, we can clear any previous validation error.
    if (this.state.issues) {
      this.state.issues.queryError = undefined;
    }

    for (const prevNode of this.prevNodes) {
      if (!prevNode.validate()) {
        if (!this.state.issues) this.state.issues = new NodeIssues();
        this.state.issues.queryError =
          prevNode.state.issues?.queryError ??
          new Error(`Previous node '${prevNode.getTitle()}' is invalid`);
        return false;
      }
    }

    return true;
  }

  getTitle(): string {
    return 'Union';
  }

  nodeDetails(): m.Child {
    const cards: m.Child[] = [];
    const selectedCols = this.state.selectedColumns.filter((c) => c.checked);
    if (selectedCols.length > 0) {
      const selectedItems = selectedCols.map((c) => {
        return m('div', c.column.name);
      });
      cards.push(
        m(Card, {className: 'pf-node-details-card'}, ...selectedItems),
      );
    }

    if (cards.length === 0) {
      return m('.pf-node-details-message', 'No common columns');
    }

    return m(CardStack, cards);
  }

  nodeSpecificModify(): m.Child {
    this.validate();
    const error = this.state.issues?.queryError;

    return m(
      '.pf-exp-query-operations',
      error && m(Callout, {icon: 'error'}, error.message),
      m(
        CardStack,
        m(
          Card,
          m('h2.pf-columns-box-title', 'Selected Columns'),
          m(
            'div.pf-column-list',
            this.state.selectedColumns.map((col, index) =>
              this.renderSelectedColumn(col, index),
            ),
          ),
        ),
      ),
    );
  }

  private renderSelectedColumn(col: ColumnInfo, index: number): m.Child {
    return m(
      '.pf-column',
      m(Checkbox, {
        checked: col.checked,
        label: col.column.name,
        onchange: (e) => {
          const newSelectedColumns = [...this.state.selectedColumns];
          newSelectedColumns[index] = {
            ...newSelectedColumns[index],
            checked: (e.target as HTMLInputElement).checked,
          };
          this.state.selectedColumns = newSelectedColumns;
          this.state.onchange?.();
        },
      }),
    );
  }

  clone(): QueryNode {
    const stateCopy: UnionNodeState = {
      prevNodes: [...this.state.prevNodes],
      onExecute: this.state.onExecute,
      selectedColumns: this.state.selectedColumns.map((c) => ({...c})),
    };
    const clone = new UnionNode(stateCopy);
    clone.filters = this.filters ? [...this.filters] : undefined;
    clone.comment = this.comment;
    return clone;
  }

  getStructuredQuery(): protos.PerfettoSqlStructuredQuery | undefined {
    return undefined;
  }

  serializeState(): UnionSerializedState {
    return {
      unionNodes: this.prevNodes.slice(1).map((n) => n.nodeId),
      selectedColumns: this.state.selectedColumns,
      filters: this.filters,
      comment: this.comment,
    };
  }

  static deserializeState(
    nodes: Map<string, QueryNode>,
    state: UnionSerializedState,
    baseNode: QueryNode,
  ): {prevNodes: QueryNode[]; selectedColumns: ColumnInfo[]} {
    const unionNodes = state.unionNodes
      .map((id) => nodes.get(id))
      .filter((node): node is QueryNode => node !== undefined);
    return {
      prevNodes: [baseNode, ...unionNodes],
      selectedColumns: state.selectedColumns,
    };
  }
}
