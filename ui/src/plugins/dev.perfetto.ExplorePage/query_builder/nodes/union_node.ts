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
  SecondaryInputSpec,
} from '../../query_node';
import {notifyNextNodes} from '../graph_utils';
import protos from '../../../../protos';
import {ColumnInfo, newColumnInfoList} from '../column_info';
import {Callout} from '../../../../widgets/callout';
import {NodeIssues} from '../node_issues';
import {Card, CardStack} from '../../../../widgets/card';
import {Checkbox} from '../../../../widgets/checkbox';
import {StructuredQueryBuilder, ColumnSpec} from '../structured_query_builder';
import {loadNodeDoc} from '../node_doc_loader';
import {NodeModifyAttrs, NodeDetailsAttrs} from '../node_explorer_types';
import {DraggableItem, SelectDeselectAllButtons} from '../widgets';
import {
  NodeDetailsMessage,
  NodeTitle,
  ColumnName,
} from '../node_styling_widgets';

export interface UnionSerializedState {
  unionNodes: string[];
  selectedColumns: ColumnInfo[];
  comment?: string;
}

export interface UnionNodeState extends QueryNodeState {
  inputNodes: QueryNode[];
  selectedColumns: ColumnInfo[];
}

export class UnionNode implements QueryNode {
  readonly nodeId: string;
  readonly type = NodeType.kUnion;
  secondaryInputs: SecondaryInputSpec;
  nextNodes: QueryNode[];
  readonly state: UnionNodeState;
  comment?: string;

  get inputNodesList(): QueryNode[] {
    return [...this.secondaryInputs.connections.entries()]
      .sort(([a], [b]) => a - b)
      .map(([, node]) => node);
  }

  get finalCols(): ColumnInfo[] {
    return this.state.selectedColumns.filter((col) => col.checked);
  }

  constructor(state: UnionNodeState) {
    this.nodeId = nextNodeId();
    this.state = {
      ...state,
      autoExecute: state.autoExecute ?? false,
    };
    this.secondaryInputs = {
      connections: new Map(),
      min: 2,
      max: 'unbounded',
      portNames: (portIndex: number) => `Input ${portIndex}`,
    };
    // Initialize connections from state.inputNodes
    for (let i = 0; i < state.inputNodes.length; i++) {
      this.secondaryInputs.connections.set(i, state.inputNodes[i]);
    }
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
    if (this.inputNodesList.length === 0) {
      return [];
    }
    // Filter out undefined entries before processing
    const validPrevNodes = this.inputNodesList.filter(
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
    const validPrevNodes = this.inputNodesList.filter(
      (node): node is QueryNode => node !== undefined,
    );

    if (validPrevNodes.length < this.inputNodesList.length) {
      this.setValidationError(
        'Union node has disconnected inputs. Please connect all inputs or remove this node.',
      );
      return false;
    }

    if (this.inputNodesList.length < 2) {
      this.setValidationError('Union node requires at least two sources.');
      return false;
    }

    if (this.getCommonColumns().length === 0) {
      this.setValidationError(
        'Union node requires common columns between sources.',
      );
      return false;
    }

    for (const inputNode of this.inputNodesList) {
      // Skip undefined entries (already handled above)
      if (inputNode === undefined) continue;

      if (!inputNode.validate()) {
        this.setValidationError(
          inputNode.state.issues?.queryError?.message ??
            `Input node '${inputNode.getTitle()}' is invalid`,
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
    return loadNodeDoc('union');
  }

  nodeDetails(): NodeDetailsAttrs {
    const selectedCols = this.state.selectedColumns.filter((c) => c.checked);

    if (selectedCols.length === 0) {
      return {
        content: [
          NodeTitle(this.getTitle()),
          NodeDetailsMessage('No common columns'),
        ],
      };
    }

    const cards: m.Child[] = [];
    // If more than 3 columns, just show the count
    if (selectedCols.length > 3) {
      cards.push(m(Card, m('div', `${selectedCols.length} common columns`)));
    } else {
      // Show individual column names for 3 or fewer
      const selectedItems = selectedCols.map((c) =>
        m('div', ColumnName(c.column.name)),
      );
      cards.push(m(Card, ...selectedItems));
    }

    return {
      content: [NodeTitle(this.getTitle()), m(CardStack, cards)],
    };
  }

  nodeSpecificModify(): NodeModifyAttrs {
    this.validate();
    const error = this.state.issues?.queryError;

    const selectedCount = this.state.selectedColumns.filter(
      (col) => col.checked,
    ).length;
    const totalCount = this.state.selectedColumns.length;

    const sections: NodeModifyAttrs['sections'] = [];

    // Add error if present
    if (error) {
      sections.push({
        content: m(Callout, {icon: 'error'}, error.message),
      });
    }

    // Selected columns section
    sections.push({
      title: `Select Common Columns (${selectedCount} / ${totalCount} selected)`,
      content: m(
        '.pf-modify-columns-content',
        m(SelectDeselectAllButtons, {
          onSelectAll: () => {
            this.state.selectedColumns = this.state.selectedColumns.map(
              (col) => ({
                ...col,
                checked: true,
              }),
            );
            this.state.onchange?.();
          },
          onDeselectAll: () => {
            this.state.selectedColumns = this.state.selectedColumns.map(
              (col) => ({
                ...col,
                checked: false,
              }),
            );
            this.state.onchange?.();
          },
        }),
        m(
          '.pf-modify-columns-node',
          m(
            '.pf-column-list-container',
            m(
              '.pf-column-list-help',
              'Select which common columns to include in the union',
            ),
            m(
              '.pf-column-list',
              this.state.selectedColumns.map((col, index) =>
                this.renderSelectedColumn(col, index),
              ),
            ),
          ),
        ),
      ),
    });

    return {
      info: 'Stacks rows from multiple inputs vertically (UNION ALL). All inputs must have compatible column schemas. Useful for combining similar data from different sources.',
      sections,
    };
  }

  private renderSelectedColumn(col: ColumnInfo, index: number): m.Child {
    return m(
      DraggableItem,
      {
        index,
        onReorder: (from: number, to: number) => {
          const newSelectedColumns = [...this.state.selectedColumns];
          const [removed] = newSelectedColumns.splice(from, 1);
          newSelectedColumns.splice(to, 0, removed);
          this.state.selectedColumns = newSelectedColumns;
          this.state.onchange?.();
        },
      },
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
      inputNodes: [...this.state.inputNodes],
      selectedColumns: this.state.selectedColumns.map((c) => ({...c})),
    };
    const clone = new UnionNode(stateCopy);
    clone.comment = this.comment;
    return clone;
  }

  getStructuredQuery(): protos.PerfettoSqlStructuredQuery | undefined {
    if (this.inputNodesList.length < 2) return undefined;

    // Check for undefined entries
    for (const inputNode of this.inputNodesList) {
      if (inputNode === undefined) return undefined;
    }

    // Get the list of checked common columns
    const selectedColumns = this.state.selectedColumns.filter((c) => c.checked);
    if (selectedColumns.length === 0) return undefined;

    // Build column specifications for the SELECT
    const columnSpecs: ColumnSpec[] = selectedColumns.map((col) => ({
      columnNameOrExpression: col.column.name,
    }));

    // Create wrapper queries for each input that selects only the common columns
    const wrappedNodes: QueryNode[] = [];
    for (const inputNode of this.inputNodesList) {
      // Create a temporary wrapper that selects only common columns
      const wrapper = {
        getStructuredQuery: () =>
          StructuredQueryBuilder.withSelectColumns(
            inputNode,
            columnSpecs,
            undefined,
          ),
      } as QueryNode;
      wrappedNodes.push(wrapper);
    }

    // Create the union from the wrapped queries
    return StructuredQueryBuilder.withUnion(wrappedNodes, true, this.nodeId);
  }

  serializeState(): UnionSerializedState {
    return {
      // Store ALL input node IDs for reliable deserialization
      unionNodes: this.inputNodesList.map((n) => n.nodeId),
      selectedColumns: this.state.selectedColumns,
      comment: this.comment,
    };
  }

  static deserializeState(state: UnionSerializedState): UnionNodeState {
    return {
      inputNodes: [],
      selectedColumns: state.selectedColumns,
    };
  }

  static deserializeConnections(
    nodes: Map<string, QueryNode>,
    state: UnionSerializedState,
  ): {inputNodes: QueryNode[]} {
    // Resolve all input nodes from their IDs
    const inputNodes = state.unionNodes
      .map((id) => nodes.get(id))
      .filter((node): node is QueryNode => node !== undefined);
    return {
      inputNodes,
    };
  }
}
