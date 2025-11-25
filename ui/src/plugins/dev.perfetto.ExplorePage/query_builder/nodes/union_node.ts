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
} from '../../query_node';
import protos from '../../../../protos';
import {ColumnInfo, newColumnInfoList} from '../column_info';
import {Callout} from '../../../../widgets/callout';
import {NodeIssues} from '../node_issues';
import {Card, CardStack} from '../../../../widgets/card';
import {Checkbox} from '../../../../widgets/checkbox';
import {StructuredQueryBuilder} from '../structured_query_builder';

export interface UnionSerializedState {
  unionNodes: string[];
  selectedColumns: ColumnInfo[];
  comment?: string;
}

export interface UnionNodeState extends QueryNodeState {
  readonly prevNodes: QueryNode[];
  selectedColumns: ColumnInfo[];
}

export class UnionNode implements MultiSourceNode {
  readonly nodeId: string;
  readonly type = NodeType.kUnion;
  readonly prevNodes: QueryNode[];
  nextNodes: QueryNode[];
  readonly state: UnionNodeState;
  comment?: string;

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
    // Filter out undefined entries before processing
    const validPrevNodes = this.prevNodes.filter(
      (node): node is QueryNode => node !== undefined,
    );
    if (validPrevNodes.length === 0) {
      return [];
    }
    let commonCols = newColumnInfoList(validPrevNodes[0].finalCols, true);
    for (let i = 1; i < validPrevNodes.length; i++) {
      const currentNodeCols = validPrevNodes[i].finalCols;
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
    // Clear any previous errors at the start of validation
    if (this.state.issues) {
      this.state.issues.clear();
    }

    // Check for undefined entries (disconnected inputs)
    const validPrevNodes = this.prevNodes.filter(
      (node): node is QueryNode => node !== undefined,
    );

    if (validPrevNodes.length < this.prevNodes.length) {
      this.setValidationError(
        'Union node has disconnected inputs. Please connect all inputs or remove this node.',
      );
      return false;
    }

    if (this.prevNodes.length < 2) {
      this.setValidationError('Union node requires at least two sources.');
      return false;
    }

    if (this.getCommonColumns().length === 0) {
      this.setValidationError(
        'Union node requires common columns between sources.',
      );
      return false;
    }

    for (const prevNode of this.prevNodes) {
      // Skip undefined entries (already handled above)
      if (prevNode === undefined) continue;

      if (!prevNode.validate()) {
        this.setValidationError(
          prevNode.state.issues?.queryError?.message ??
            `Previous node '${prevNode.getTitle()}' is invalid`,
        );
        return false;
      }
    }

    return true;
  }

  private setValidationError(message: string): void {
    if (!this.state.issues) {
      this.state.issues = new NodeIssues();
    }
    this.state.issues.queryError = new Error(message);
  }

  getTitle(): string {
    return 'Union';
  }

  nodeInfo(): m.Children {
    return m(
      'div',
      m(
        'p',
        'Stack rows from multiple sources into a single result. All connected sources must have compatible column names and types.',
      ),
      m(
        'p',
        'Select which common columns to include in the result. Connect at least two sources to the input ports.',
      ),
      m(
        'p',
        m('strong', 'Example:'),
        ' Combine CPU slices from multiple processes to analyze them together.',
      ),
    );
  }

  nodeDetails(): m.Child {
    const cards: m.Child[] = [];
    const selectedCols = this.state.selectedColumns.filter((c) => c.checked);
    if (selectedCols.length > 0) {
      // If more than 3 columns, just show the count
      if (selectedCols.length > 3) {
        cards.push(
          m(
            Card,
            {className: 'pf-node-details-card'},
            m('div', `${selectedCols.length} common columns`),
          ),
        );
      } else {
        // Show individual column names for 3 or fewer
        const selectedItems = selectedCols.map((c) => {
          return m('div', c.column.name);
        });
        cards.push(
          m(Card, {className: 'pf-node-details-card'}, ...selectedItems),
        );
      }
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
      selectedColumns: this.state.selectedColumns.map((c) => ({...c})),
    };
    const clone = new UnionNode(stateCopy);
    clone.comment = this.comment;
    return clone;
  }

  getStructuredQuery(): protos.PerfettoSqlStructuredQuery | undefined {
    if (this.prevNodes.length < 2) return undefined;

    // Check for undefined entries
    for (const prevNode of this.prevNodes) {
      if (prevNode === undefined) return undefined;
    }

    return StructuredQueryBuilder.withUnion(this.prevNodes, true, this.nodeId);
  }

  serializeState(): UnionSerializedState {
    return {
      unionNodes: this.prevNodes.slice(1).map((n) => n.nodeId),
      selectedColumns: this.state.selectedColumns,
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
