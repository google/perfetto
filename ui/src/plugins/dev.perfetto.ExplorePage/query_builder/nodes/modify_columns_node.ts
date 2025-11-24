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
  notifyNextNodes,
  ModificationNode,
} from '../../query_node';
import {Button, ButtonVariant} from '../../../../widgets/button';
import {Card, CardStack} from '../../../../widgets/card';
import {Checkbox} from '../../../../widgets/checkbox';
import {TextInput} from '../../../../widgets/text_input';
import {ColumnInfo, newColumnInfoList} from '../column_info';
import protos from '../../../../protos';
import {NodeIssues} from '../node_issues';
import {StructuredQueryBuilder, ColumnSpec} from '../structured_query_builder';

export interface ModifyColumnsSerializedState {
  prevNodeId?: string;
  selectedColumns: {
    name: string;
    type: string;
    checked: boolean;
    alias?: string;
  }[];
  comment?: string;
}

export interface ModifyColumnsState extends QueryNodeState {
  prevNode: QueryNode;
  selectedColumns: ColumnInfo[];
}

export class ModifyColumnsNode implements ModificationNode {
  readonly nodeId: string;
  readonly type = NodeType.kModifyColumns;
  readonly prevNode: QueryNode;
  nextNodes: QueryNode[];
  readonly state: ModifyColumnsState;

  constructor(state: ModifyColumnsState) {
    this.nodeId = nextNodeId();
    this.prevNode = state.prevNode;
    this.nextNodes = [];

    this.state = {
      ...state,
      selectedColumns: state.selectedColumns ?? [],
    };

    if (
      this.state.selectedColumns.length === 0 &&
      this.prevNode !== undefined
    ) {
      this.state.selectedColumns = newColumnInfoList(this.prevNode.finalCols);
    }

    const userOnChange = this.state.onchange;
    this.state.onchange = () => {
      notifyNextNodes(this);
      userOnChange?.();
    };
  }

  get finalCols(): ColumnInfo[] {
    return this.computeFinalCols();
  }

  private computeFinalCols(): ColumnInfo[] {
    const finalCols = newColumnInfoList(
      this.state.selectedColumns.filter((col) => col.checked),
    );
    return finalCols;
  }

  onPrevNodesUpdated() {
    // This node assumes it has only one previous node.
    if (this.prevNode === undefined) {
      return;
    }

    const sourceCols = this.prevNode.finalCols;

    const newSelectedColumns = newColumnInfoList(sourceCols);

    // Preserve checked status and aliases for columns that still exist.
    for (const oldCol of this.state.selectedColumns) {
      const newCol = newSelectedColumns.find(
        (c) => c.column.name === oldCol.column.name,
      );
      if (newCol) {
        newCol.checked = oldCol.checked;
        newCol.alias = oldCol.alias;
      }
    }

    this.state.selectedColumns = newSelectedColumns;
  }

  static deserializeState(
    serializedState: ModifyColumnsSerializedState,
  ): ModifyColumnsState {
    return {
      ...serializedState,
      prevNode: undefined as unknown as QueryNode,
      selectedColumns: serializedState.selectedColumns.map((c) => ({
        name: c.name,
        type: c.type,
        checked: c.checked,
        column: {name: c.name},
        alias: c.alias,
      })),
    };
  }

  resolveColumns() {
    // Recover full column information from prevNode
    if (this.prevNode === undefined) {
      return;
    }

    const sourceCols = this.prevNode.finalCols ?? [];
    this.state.selectedColumns.forEach((c) => {
      const sourceCol = sourceCols.find((s) => s.name === c.name);
      if (sourceCol) {
        c.column = sourceCol.column;
        c.type = sourceCol.type;
      }
    });
  }

  validate(): boolean {
    // Clear any previous errors at the start of validation
    if (this.state.issues) {
      this.state.issues.clear();
    }

    const colNames = new Set<string>();
    for (const col of this.state.selectedColumns) {
      if (!col.checked) continue;
      // Check for empty or whitespace-only alias
      if (col.alias !== undefined && col.alias.trim() === '') {
        this.setValidationError('Empty alias not allowed');
        return false;
      }
      const name = col.alias ? col.alias.trim() : col.column.name;
      if (colNames.has(name)) {
        this.setValidationError('Duplicate column names');
        return false;
      }
      colNames.add(name);
    }

    // Check if there are no columns selected
    if (colNames.size === 0) {
      this.setValidationError(
        'No columns selected. Select at least one column.',
      );
      return false;
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
    return 'Modify Columns';
  }

  nodeDetails(): m.Child {
    // Determine the state of modifications.
    const hasUnselected = this.state.selectedColumns.some((c) => !c.checked);
    const hasAlias = this.state.selectedColumns.some((c) => c.alias);
    if (!hasUnselected && !hasAlias) {
      return m('.pf-exp-node-details-message', 'Select all');
    }

    const cards: m.Child[] = [];

    // If columns have been unselected or aliased, list the selected ones.
    if (hasUnselected || hasAlias) {
      const selectedCols = this.state.selectedColumns.filter((c) => c.checked);
      if (selectedCols.length > 0) {
        // If there are too many selected columns and some are unselected, show a summary.
        const maxColumnsToShow = 5;
        const shouldShowSummary =
          hasUnselected && selectedCols.length > maxColumnsToShow;

        if (shouldShowSummary) {
          const renamedCols = selectedCols.filter((c) => c.alias);
          const totalCols = this.state.selectedColumns.length;
          const summaryText = `${selectedCols.length} of ${totalCols} columns selected`;

          // Show up to 3 renamed columns explicitly even in summary mode
          if (renamedCols.length > 0 && renamedCols.length <= 3) {
            const renamedItems = renamedCols.map((c) =>
              m('div', `${c.column.name} AS ${c.alias}`),
            );
            cards.push(
              m(
                Card,
                {className: 'pf-exp-node-details-card'},
                m('div', summaryText),
                m('div', {style: 'height: 8px'}), // spacing
                ...renamedItems,
              ),
            );
          } else {
            cards.push(
              m(
                Card,
                {className: 'pf-exp-node-details-card'},
                m('div', summaryText),
              ),
            );
          }
        } else {
          const selectedItems = selectedCols.map((c) => {
            if (c.alias) {
              return m('div', `${c.column.name} AS ${c.alias}`);
            } else {
              return m('div', c.column.name);
            }
          });
          cards.push(
            m(Card, {className: 'pf-exp-node-details-card'}, ...selectedItems),
          );
        }
      }
    }

    // If all columns have been deselected, show a specific message.
    if (cards.length === 0) {
      return m('.pf-exp-node-details-message', 'All columns deselected');
    }

    return m(CardStack, cards);
  }

  nodeSpecificModify(): m.Child {
    return m(
      'div.pf-modify-columns-node',
      this.renderHeader(),
      this.renderColumnList(),
    );
  }

  private renderHeader(): m.Child {
    const selectedCount = this.state.selectedColumns.filter(
      (col) => col.checked,
    ).length;
    const totalCount = this.state.selectedColumns.length;

    return m(
      '.pf-modify-columns-header',
      m('.pf-modify-columns-title', 'Select and Rename Columns'),
      m(
        '.pf-modify-columns-actions',
        m(
          '.pf-modify-columns-stats',
          `${selectedCount} / ${totalCount} selected`,
        ),
        m(
          '.pf-modify-columns-buttons',
          m(Button, {
            label: 'Select All',
            variant: ButtonVariant.Outlined,
            compact: true,
            onclick: () => {
              this.state.selectedColumns = this.state.selectedColumns.map(
                (col) => ({
                  ...col,
                  checked: true,
                }),
              );
              this.state.onchange?.();
            },
          }),
          m(Button, {
            label: 'Deselect All',
            variant: ButtonVariant.Outlined,
            compact: true,
            onclick: () => {
              this.state.selectedColumns = this.state.selectedColumns.map(
                (col) => ({
                  ...col,
                  checked: false,
                }),
              );
              this.state.onchange?.();
            },
          }),
        ),
      ),
    );
  }

  private renderColumnList(): m.Child {
    return m(
      '.pf-column-list-container',
      m(
        '.pf-column-list-help',
        'Check columns to include, add aliases to rename, and drag to reorder',
      ),
      m(
        '.pf-column-list',
        this.state.selectedColumns.map((col, index) =>
          this.renderSelectedColumn(col, index),
        ),
      ),
    );
  }

  private renderSelectedColumn(col: ColumnInfo, index: number): m.Child {
    return m(
      '.pf-column',
      {
        ondragover: (e: DragEvent) => {
          e.preventDefault();
        },
        ondrop: (e: DragEvent) => {
          e.preventDefault();
          const from = parseInt(e.dataTransfer!.getData('text/plain'), 10);
          const to = index;

          const newSelectedColumns = [...this.state.selectedColumns];
          const [removed] = newSelectedColumns.splice(from, 1);
          newSelectedColumns.splice(to, 0, removed);
          this.state.selectedColumns = newSelectedColumns;
          this.state.onchange?.();
        },
      },
      m(
        'span.pf-drag-handle',
        {
          draggable: true,
          ondragstart: (e: DragEvent) => {
            e.dataTransfer!.setData('text/plain', index.toString());
          },
        },
        '☰',
      ),
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
      m(TextInput, {
        oninput: (e: Event) => {
          const newSelectedColumns = [...this.state.selectedColumns];
          newSelectedColumns[index] = {
            ...newSelectedColumns[index],
            alias: (e.target as HTMLInputElement).value,
          };
          this.state.selectedColumns = newSelectedColumns;
          this.state.onchange?.();
        },
        placeholder: 'alias',
        value: col.alias ? col.alias : '',
      }),
    );
  }

  nodeInfo(): m.Children {
    return m(
      'div',
      m(
        'p',
        'Select which columns to include from the previous node, rename columns using aliases, and reorder columns using drag and drop.',
      ),
      m(
        'p',
        m('strong', 'Example:'),
        ' Select only ',
        m('code', 'id'),
        ' and ',
        m('code', 'ts'),
        ' columns, and rename ',
        m('code', 'ts'),
        ' to ',
        m('code', 'timestamp'),
        ' using an alias.',
      ),
    );
  }

  clone(): QueryNode {
    return new ModifyColumnsNode(this.state);
  }

  getStructuredQuery(): protos.PerfettoSqlStructuredQuery | undefined {
    if (this.prevNode === undefined) return undefined;

    // Build column specifications
    const columns: ColumnSpec[] = [];

    for (const col of this.state.selectedColumns) {
      if (!col.checked) continue;
      columns.push({
        columnNameOrExpression: col.column.name,
        alias: col.alias,
      });
    }

    // Apply column selection
    return StructuredQueryBuilder.withSelectColumns(
      this.prevNode,
      columns,
      undefined,
      this.nodeId,
    );
  }

  serializeState(): ModifyColumnsSerializedState {
    return {
      prevNodeId: this.prevNode?.nodeId,
      selectedColumns: this.state.selectedColumns.map((c) => ({
        name: c.name,
        type: c.type,
        checked: c.checked,
        alias: c.alias,
      })),
      comment: this.state.comment,
    };
  }
}
