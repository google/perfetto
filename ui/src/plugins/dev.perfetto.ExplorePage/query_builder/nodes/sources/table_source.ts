// Copyright (C) 2025 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

import m from 'mithril';
import {
  SqlTable,
  SqlModules,
} from '../../../../dev.perfetto.SqlModules/sql_modules';
import {
  QueryNode,
  createSelectColumnsProto,
  QueryNodeState,
  NodeType,
  createFinalColumns,
  SourceNode,
  nextNodeId,
} from '../../../query_node';
import {ColumnInfo, columnInfoFromSqlColumn} from '../../column_info';
import protos from '../../../../../protos';
import {TextParagraph} from '../../../../../widgets/text_paragraph';
import {Button} from '../../../../../widgets/button';
import {Trace} from '../../../../../public/trace';
import {
  createFiltersProto,
  FilterOperation,
  UIFilter,
} from '../../operations/filter';
import {closeModal, showModal} from '../../../../../widgets/modal';
import {TableList} from '../../table_list';
import {redrawModal} from '../../../../../widgets/modal';

export interface TableSourceSerializedState {
  sqlTable?: string;
  filters?: UIFilter[];
  customTitle?: string;
  comment?: string;
}

export interface TableSourceState extends QueryNodeState {
  readonly trace: Trace;
  readonly sqlModules: SqlModules;

  sqlTable?: SqlTable;
  onchange?: () => void;
}

interface TableSelectionResult {
  sqlTable: SqlTable;
  sourceCols: ColumnInfo[];
}

export function modalForTableSelection(
  sqlModules: SqlModules,
): Promise<TableSelectionResult | undefined> {
  return new Promise((resolve) => {
    let searchQuery = '';

    showModal({
      title: 'Choose a table',
      content: () => {
        return m(
          '.pf-exp-node-explorer-help',
          m(TableList, {
            sqlModules,
            onTableClick: (tableName: string) => {
              const sqlTable = sqlModules.getTable(tableName);
              if (!sqlTable) {
                resolve(undefined);
                return;
              }
              const sourceCols = sqlTable.columns.map((c) =>
                columnInfoFromSqlColumn(c, true),
              );
              resolve({sqlTable, sourceCols});
              closeModal();
            },
            searchQuery,
            onSearchQueryChange: (query) => {
              searchQuery = query;
              redrawModal();
            },
            autofocus: true,
          }),
        );
      },
      buttons: [],
    });
  });
}

export class TableSourceNode implements SourceNode {
  readonly nodeId: string;
  readonly state: TableSourceState;
  readonly prevNodes: QueryNode[] = [];
  showColumns: boolean = false;
  readonly finalCols: ColumnInfo[];
  nextNodes: QueryNode[];

  constructor(attrs: TableSourceState) {
    this.nodeId = nextNodeId();
    this.state = attrs;
    this.state.onchange = attrs.onchange;
    this.finalCols = createFinalColumns(
      this.state.sqlTable?.columns.map((c) =>
        columnInfoFromSqlColumn(c, true),
      ) ?? [],
    );
    this.nextNodes = [];

    this.state.filters = attrs.filters ?? [];
  }

  get type() {
    return NodeType.kTable;
  }

  clone(): QueryNode {
    const stateCopy: TableSourceState = {
      trace: this.state.trace,
      sqlModules: this.state.sqlModules,
      sqlTable: this.state.sqlTable,
      filters: this.state.filters?.map((f) => ({...f})),
      customTitle: this.state.customTitle,
      onchange: this.state.onchange,
    };
    return new TableSourceNode(stateCopy);
  }

  nodeSpecificModify(): m.Child {
    if (this.state.sqlTable != null) {
      const table = this.state.sqlTable;
      return m(
        '.pf-stdlib-table-node',
        m(
          '.pf-details-box',
          m(TextParagraph, {text: table.description}),
          m(Button, {
            label: this.showColumns ? 'Hide Columns' : 'Show Columns',
            onclick: () => {
              this.showColumns = !this.showColumns;
            },
          }),
          this.showColumns &&
            m(
              'table.pf-table.pf-table-striped',
              m(
                'thead',
                m(
                  'tr',
                  m('th', 'Column'),
                  m('th', 'Type'),
                  m('th', 'Description'),
                ),
              ),
              m(
                'tbody',
                table.columns.map((col) => {
                  return m(
                    'tr',
                    m('td', col.name),
                    m('td', col.type.name),
                    m('td', col.description),
                  );
                }),
              ),
            ),
        ),
        m(FilterOperation, {
          filters: this.state.filters,
          sourceCols: this.finalCols,
          onFiltersChanged: (newFilters: ReadonlyArray<UIFilter>) => {
            this.state.filters = [...newFilters];
            this.state.onchange?.();
          },
        }),
      );
    }
    return m(TextParagraph, 'No description available for this table.');
  }

  validate(): boolean {
    return this.state.sqlTable !== undefined;
  }

  getTitle(): string {
    return this.state.customTitle ?? `${this.state.sqlTable?.name}`;
  }

  getStructuredQuery(): protos.PerfettoSqlStructuredQuery | undefined {
    if (!this.validate()) return;
    if (!this.state.sqlTable) return;

    const sq = new protos.PerfettoSqlStructuredQuery();
    sq.id = this.nodeId;
    sq.table = new protos.PerfettoSqlStructuredQuery.Table();
    sq.table.tableName = this.state.sqlTable.name;
    sq.table.moduleName = this.state.sqlTable.includeKey
      ? this.state.sqlTable.includeKey
      : undefined;
    sq.table.columnNames = this.finalCols
      .filter((c) => c.checked)
      .map((c) => c.column.name);

    const filtersProto = createFiltersProto(this.state.filters, this.finalCols);
    if (filtersProto) sq.filters = filtersProto;

    const selectedColumns = createSelectColumnsProto(this);
    if (selectedColumns) sq.selectColumns = selectedColumns;
    return sq;
  }

  serializeState(): TableSourceSerializedState {
    return {
      sqlTable: this.state.sqlTable?.name,
      filters: this.state.filters,
      customTitle: this.state.customTitle,
      comment: this.state.comment,
    };
  }

  static deserializeState(
    trace: Trace,
    sqlModules: SqlModules,
    state: TableSourceSerializedState,
  ): TableSourceState {
    const sqlTable = state.sqlTable
      ? sqlModules.getTable(state.sqlTable)
      : undefined;
    return {
      ...state,
      trace,
      sqlModules,
      sqlTable,
    };
  }
}
