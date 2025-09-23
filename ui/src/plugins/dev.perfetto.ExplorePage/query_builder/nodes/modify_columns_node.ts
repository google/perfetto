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
} from '../../query_node';
import {Checkbox} from '../../../../widgets/checkbox';
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
  prevNodeIds: string[];
  newColumns: NewColumn[];
  selectedColumns: ColumnInfo[];
  filters: FilterDefinition[];
  customTitle?: string;
}

export interface ModifyColumnsState extends QueryNodeState {
  prevNodes: QueryNode[];
  newColumns: NewColumn[];
  selectedColumns: ColumnInfo[];
  filters: FilterDefinition[];
  customTitle?: string;
}

export class ModifyColumnsNode implements QueryNode {
  readonly nodeId: string;
  readonly type = NodeType.kModifyColumns;
  prevNodes: QueryNode[] | undefined;
  nextNodes: QueryNode[];
  sourceCols: ColumnInfo[];
  readonly state: ModifyColumnsState;

  constructor(state: ModifyColumnsState) {
    this.nodeId = nextNodeId();
    // This node assumes it has only one previous node.
    this.sourceCols =
      state.prevNodes.length > 0 ? state.prevNodes[0].finalCols : [];
    if (state.selectedColumns.length === 0 && this.sourceCols.length > 0) {
      state.selectedColumns = newColumnInfoList(this.sourceCols);
    }
    this.prevNodes = state.prevNodes;
    this.nextNodes = [];
    this.state = state;
  }

  onPrevNodesUpdated() {
    // This node assumes it has only one previous node.
    this.sourceCols =
      this.state.prevNodes.length > 0 ? this.state.prevNodes[0].finalCols : [];
    if (this.state.selectedColumns.length === 0 && this.sourceCols.length > 0) {
      this.state.selectedColumns = newColumnInfoList(this.sourceCols);
    }
  }

  static deserializeState(
    nodes: Map<string, QueryNode>,
    serializedState: ModifyColumnsSerializedState,
  ): ModifyColumnsState {
    const prevNodes = serializedState.prevNodeIds.map((id) => nodes.get(id)!);
    return {
      ...serializedState,
      prevNodes,
    };
  }

  get finalCols(): ColumnInfo[] {
    const finalCols = newColumnInfoList(
      this.state.selectedColumns.filter((col) => col.checked),
    );
    this.state.newColumns.forEach((col) => {
      finalCols.push(columnInfoFromName(col.name, true));
    });
    return finalCols;
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

  nodeSpecificModify(): m.Child {
    return m(
      'div',
      m(
        'div.pf-column-list',
        this.state.selectedColumns.map((col, index) =>
          m(
            '.pf-column',
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
                const from = parseInt(
                  e.dataTransfer!.getData('text/plain'),
                  10,
                );
                const to = index;
                const [removed] = this.state.selectedColumns.splice(from, 1);
                this.state.selectedColumns.splice(to, 0, removed);
              },
            },
            m(Checkbox, {
              checked: col.checked,
              label: col.column.name,
              onchange: (e) => {
                col.checked = (e.target as HTMLInputElement).checked;
              },
            }),
            m(TextInput, {
              oninput: (e: Event) => {
                col.alias = (e.target as HTMLInputElement).value;
              },
              placeholder: 'alias',
              value: col.alias ? col.alias : '',
            }),
          ),
        ),
      ),
      this.state.newColumns.map((col, index) =>
        m(
          '.pf-column',
          {
            draggable: true,
            ondragstart: (e: DragEvent) => {
              e.dataTransfer!.setData(
                'text/plain',
                (this.state.selectedColumns.length + index).toString(),
              );
            },
            ondragover: (e: DragEvent) => {
              e.preventDefault();
            },
            ondrop: (e: DragEvent) => {
              e.preventDefault();
              const from = parseInt(e.dataTransfer!.getData('text/plain'), 10);
              const to = this.state.selectedColumns.length + index;
              if (from < this.state.selectedColumns.length) {
                const [removed] = this.state.selectedColumns.splice(from, 1);
                this.state.newColumns.splice(
                  to - this.state.selectedColumns.length,
                  0,
                  {
                    expression: removed.column.name,
                    name: removed.alias || '',
                  },
                );
              } else {
                const [removed] = this.state.newColumns.splice(
                  from - this.state.selectedColumns.length,
                  1,
                );
                this.state.newColumns.splice(
                  to - this.state.selectedColumns.length,
                  0,
                  removed,
                );
              }
            },
          },
          m(TextInput, {
            oninput: (e: Event) => {
              col.expression = (e.target as HTMLInputElement).value;
            },
            placeholder: 'expression',
            value: col.expression,
          }),
          m(TextInput, {
            oninput: (e: Event) => {
              col.name = (e.target as HTMLInputElement).value;
            },
            placeholder: 'name',
            value: col.name,
          }),
          m(
            'button',
            {
              onclick: () => {
                this.state.newColumns.splice(index, 1);
              },
            },
            'Remove',
          ),
        ),
      ),
      m(
        'button.pf-add-column-button',
        {
          onclick: () => {
            this.state.newColumns.push({
              expression: '',
              name: '',
            });
          },
        },
        'Add column',
      ),
      m(FilterOperation, {
        filters: this.state.filters,
        sourceCols: this.finalCols,
        onFiltersChanged: (newFilters: ReadonlyArray<FilterDefinition>) => {
          this.state.filters = newFilters as FilterDefinition[];
          this.state.onchange?.();
        },
      }),
    );
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

    if (!this.prevNodes || this.prevNodes.length === 0) return;
    // This node assumes it has only one previous node.
    const prevSq = this.prevNodes[0].getStructuredQuery();
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
    return {
      prevNodeIds: this.prevNodes!.map((node) => node.nodeId),
      newColumns: this.state.newColumns,
      selectedColumns: this.state.selectedColumns,
      filters: this.state.filters,
      customTitle: this.state.customTitle,
    };
  }
}
