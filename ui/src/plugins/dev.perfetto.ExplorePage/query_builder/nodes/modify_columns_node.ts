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
import {Icon} from '../../../../widgets/icon';
import {TextInput} from '../../../../widgets/text_input';
import {
  ColumnInfo,
  columnInfoFromName,
  newColumnInfoList,
} from '../column_info';
import protos from '../../../../protos';
import {FilterDefinition} from '../../../../components/widgets/data_grid/common';
import {createFiltersProto, FilterOperation} from '../operations/filter';

interface NewColumn {
  expression: string;
  name: string;
  module?: string;
}

export interface ModifyColumnsSerializedState {
  prevNodeId: string;
  newColumns: NewColumn[];
  selectedColumns: ColumnInfo[];
  filters?: FilterDefinition[];
  customTitle?: string;
  comment?: string;
}

export interface ModifyColumnsState extends QueryNodeState {
  prevNode: QueryNode;
  newColumns: NewColumn[];
  selectedColumns: ColumnInfo[];
  filters?: FilterDefinition[];
  customTitle?: string;
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
      newColumns: state.newColumns ?? [],
      selectedColumns: state.selectedColumns ?? [],
    };

    if (this.state.selectedColumns.length === 0) {
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
    this.state.newColumns
      .filter((c) => this.isNewColumnValid(c))
      .forEach((col) => {
        finalCols.push(columnInfoFromName(col.name, true));
      });
    return finalCols;
  }

  onPrevNodesUpdated() {
    // This node assumes it has only one previous node.
    const sourceCols = this.state.prevNode.finalCols;

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
    };
  }

  validate(): boolean {
    const colNames = new Set<string>();
    for (const col of this.state.selectedColumns) {
      if (!col.checked) continue;
      const name = col.alias ? col.alias.trim() : col.column.name;
      if (col.alias && name === '') return false; // Disallow empty alias
      if (colNames.has(name)) {
        return false;
      }
      colNames.add(name);
    }

    for (const col of this.state.newColumns) {
      const name = col.name.trim();
      const expression = col.expression.trim();

      // If a column has an expression, it must have a name and be unique.
      if (expression !== '') {
        if (name === '') {
          return false;
        }
        if (colNames.has(name)) {
          return false;
        }
        colNames.add(name);
      }
    }

    return true;
  }

  getTitle(): string {
    return this.state.customTitle ?? 'Modify Columns';
  }

  nodeDetails(): m.Child {
    // Determine the state of modifications.
    const hasUnselected = this.state.selectedColumns.some((c) => !c.checked);
    const hasAlias = this.state.selectedColumns.some((c) => c.alias);
    const newValidColumns = this.state.newColumns.filter((c) =>
      this.isNewColumnValid(c),
    );

    // If there are no modifications, show a default message.
    if (!hasUnselected && !hasAlias && newValidColumns.length === 0) {
      return m('.pf-node-details-message', 'Select all');
    }

    const cards: m.Child[] = [];

    // If columns have been unselected or aliased, list the selected ones.
    if (hasUnselected || hasAlias) {
      const selectedCols = this.state.selectedColumns.filter((c) => c.checked);
      if (selectedCols.length > 0) {
        const selectedItems = selectedCols.map((c) => {
          if (c.alias) {
            return m('div', `${c.column.name} AS ${c.alias}`);
          } else {
            return m('div', c.column.name);
          }
        });
        cards.push(
          m(Card, {className: 'pf-node-details-card'}, ...selectedItems),
        );
      }
    }

    // If new columns have been added, list them.
    if (newValidColumns.length > 0) {
      const newItems = newValidColumns.map((c) =>
        m('div', `${c.expression} AS ${c.name}`),
      );
      cards.push(m(Card, {className: 'pf-node-details-card'}, ...newItems));
    }

    // If all columns have been deselected, show a specific message.
    if (cards.length === 0) {
      return m('.pf-node-details-message', 'All columns deselected');
    }

    return m(CardStack, cards);
  }

  nodeSpecificModify(): m.Child {
    return m(
      'div.pf-modify-columns-node',
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
        m(
          Card,
          this.state.newColumns.map((col, index) =>
            this.renderNewColumn(col, index),
          ),
          this.renderAddColumnButton(),
        ),
      ),
      this.renderFilterOperation(),
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

  private isNewColumnValid(col: NewColumn): boolean {
    return col.expression.trim() !== '' && col.name.trim() !== '';
  }

  private renderNewColumn(col: NewColumn, index: number): m.Child {
    const isValid = this.isNewColumnValid(col);

    return m(
      '.pf-column',
      {
        ondragover: (e: DragEvent) => {
          e.preventDefault();
        },
        ondrop: (e: DragEvent) => {
          e.preventDefault();
          const from = parseInt(e.dataTransfer!.getData('text/plain'), 10);
          const to = this.state.selectedColumns.length + index;

          const newSelectedColumns = [...this.state.selectedColumns];
          const newNewColumns = [...this.state.newColumns];

          if (from < this.state.selectedColumns.length) {
            const [removed] = newSelectedColumns.splice(from, 1);
            newNewColumns.splice(to - this.state.selectedColumns.length, 0, {
              expression: removed.column.name,
              name: removed.alias || '',
            });
          } else {
            const [removed] = newNewColumns.splice(
              from - this.state.selectedColumns.length,
              1,
            );
            newNewColumns.splice(
              to - this.state.selectedColumns.length,
              0,
              removed,
            );
          }
          this.state.selectedColumns = newSelectedColumns;
          this.state.newColumns = newNewColumns;
          this.state.onchange?.();
        },
      },
      m(
        'span.pf-drag-handle',
        {
          draggable: true,
          ondragstart: (e: DragEvent) => {
            e.dataTransfer!.setData(
              'text/plain',
              (this.state.selectedColumns.length + index).toString(),
            );
          },
        },
        '☰',
      ),
      m(TextInput, {
        oninput: (e: Event) => {
          const newNewColumns = [...this.state.newColumns];
          newNewColumns[index] = {
            ...newNewColumns[index],
            expression: (e.target as HTMLInputElement).value,
          };
          this.state.newColumns = newNewColumns;
          this.state.onchange?.();
        },
        placeholder: 'expression',
        value: col.expression,
      }),
      m(TextInput, {
        oninput: (e: Event) => {
          const newNewColumns = [...this.state.newColumns];
          newNewColumns[index] = {
            ...newNewColumns[index],
            name: (e.target as HTMLInputElement).value,
          };
          this.state.newColumns = newNewColumns;
          this.state.onchange?.();
        },
        placeholder: 'name',
        value: col.name,
      }),
      !isValid && m(Icon, {icon: 'warning'}),
      m(
        'button',
        {
          onclick: () => {
            const newNewColumns = [...this.state.newColumns];
            newNewColumns.splice(index, 1);
            this.state.newColumns = newNewColumns;
            this.state.onchange?.();
          },
        },
        m(Icon, {icon: 'close'}),
      ),
    );
  }

  private renderAddColumnButton(): m.Child {
    return m(Button, {
      label: 'Add column',
      variant: ButtonVariant.Outlined,
      onclick: () => {
        this.state.newColumns = [
          ...this.state.newColumns,
          {
            expression: '',
            name: '',
          },
        ];
        this.state.onchange?.();
      },
    });
  }

  private renderFilterOperation(): m.Child {
    return m(FilterOperation, {
      filters: this.state.filters,
      sourceCols: this.finalCols,
      onFiltersChanged: (newFilters: ReadonlyArray<FilterDefinition>) => {
        this.state.filters = newFilters as FilterDefinition[];
        this.state.onchange?.();
      },
    });
  }

  clone(): QueryNode {
    return new ModifyColumnsNode(this.state);
  }

  getStructuredQuery(): protos.PerfettoSqlStructuredQuery | undefined {
    const selectColumns: protos.PerfettoSqlStructuredQuery.SelectColumn[] = [];
    const referencedModules: string[] = [];

    for (const col of this.state.selectedColumns) {
      if (!col.checked) continue;

      const selectColumn = new protos.PerfettoSqlStructuredQuery.SelectColumn();
      selectColumn.columnNameOrExpression = col.column.name;
      if (col.alias) {
        selectColumn.alias = col.alias;
      }
      selectColumns.push(selectColumn);
    }

    for (const col of this.state.newColumns) {
      const selectColumn = new protos.PerfettoSqlStructuredQuery.SelectColumn();
      selectColumn.columnNameOrExpression = col.expression;
      selectColumn.alias = col.name;
      selectColumns.push(selectColumn);
      if (col.module) {
        referencedModules.push(col.module);
      }
    }

    // This node assumes it has only one previous node.
    const prevSq = this.prevNode.getStructuredQuery();
    if (!prevSq) return;

    prevSq.selectColumns = selectColumns;
    if (referencedModules.length > 0) {
      prevSq.referencedModules = referencedModules;
    }

    const filtersProto = createFiltersProto(this.state.filters, this.finalCols);

    if (filtersProto) {
      const outerSq = new protos.PerfettoSqlStructuredQuery();
      outerSq.id = this.nodeId;
      outerSq.innerQuery = prevSq;
      outerSq.filters = filtersProto;
      return outerSq;
    }

    return prevSq;
  }

  isMaterialised(): boolean {
    return false;
  }

  serializeState(): ModifyColumnsSerializedState {
    if (this.prevNode === undefined) {
      throw new Error('Cannot serialize ModifyColumnsNode without a prevNode');
    }
    return {
      prevNodeId: this.prevNode.nodeId,
      newColumns: this.state.newColumns,
      selectedColumns: this.state.selectedColumns,
      filters: this.state.filters,
      customTitle: this.state.customTitle,
      comment: this.state.comment,
    };
  }
}
