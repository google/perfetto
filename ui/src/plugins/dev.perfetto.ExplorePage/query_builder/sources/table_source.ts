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
} from '../../../dev.perfetto.SqlModules/sql_modules';
import {
  QueryNode,
  createSelectColumnsProto,
  QueryNodeState,
  NodeType,
} from '../../query_node';
import {
  ColumnInfo,
  columnInfoFromSqlColumn,
  newColumnInfoList,
} from '../column_info';
import protos from '../../../../protos';
import {TextParagraph} from '../../../../widgets/text_paragraph';
import {Button} from '../../../../widgets/button';
import {Trace} from '../../../../public/trace';
import {
  createFiltersProto,
  createGroupByProto,
} from '../operations/operation_component';
import {closeModal, showModal} from '../../../../widgets/modal';
import {TableList} from '../table_list';
import {redrawModal} from '../../../../widgets/modal';
import {SourceNode} from '../source_node';

export interface TableSourceState extends QueryNodeState {
  readonly trace: Trace;
  readonly sqlModules: SqlModules;

  sqlTable?: SqlTable;
  onchange?: () => void;
}

interface TableSelectionResult {
  sqlTable: SqlTable;
  sourceCols: ColumnInfo[];
  groupByColumns: ColumnInfo[];
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
          '.pf-node-explorer-help',
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
              const groupByColumns = newColumnInfoList(sourceCols, false);
              resolve({sqlTable, sourceCols, groupByColumns});
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

export class TableSourceNode extends SourceNode {
  readonly state: TableSourceState;
  showColumns: boolean = false;

  constructor(attrs: TableSourceState) {
    super(attrs);
    this.state = attrs;
    this.state.onchange = attrs.onchange;

    this.state.filters = attrs.filters ?? [];
    this.state.groupByColumns =
      attrs.groupByColumns ?? newColumnInfoList(this.sourceCols, false);
    this.state.aggregations = attrs.aggregations ?? [];
  }

  get type() {
    return NodeType.kTable;
  }

  clone(): QueryNode {
    const stateCopy: TableSourceState = {
      trace: this.state.trace,
      sqlModules: this.state.sqlModules,
      sqlTable: this.state.sqlTable,
      sourceCols: newColumnInfoList(this.sourceCols),
      groupByColumns: newColumnInfoList(this.state.groupByColumns),
      filters: this.state.filters.map((f) => ({...f})),
      aggregations: this.state.aggregations.map((a) => ({...a})),
      customTitle: this.state.customTitle,
      onchange: this.state.onchange,
    };
    return new TableSourceNode(stateCopy);
  }

  nodeSpecificModify(): m.Child {
    if (this.state.sqlTable) {
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
      );
    }
    return m(TextParagraph, 'No description available for this table.');
  }

  validate(): boolean {
    return this.state.sqlTable !== undefined;
  }

  getTitle(): string {
    return this.state.customTitle ?? `Table ${this.state.sqlTable?.name}`;
  }

  getStructuredQuery(): protos.PerfettoSqlStructuredQuery | undefined {
    if (!this.validate()) return;
    if (!this.state.sqlTable) return;

    const sq = new protos.PerfettoSqlStructuredQuery();
    sq.id = `table_source_${this.state.sqlTable?.name}`;
    sq.table = new protos.PerfettoSqlStructuredQuery.Table();
    sq.table.tableName = this.state.sqlTable.name;
    sq.table.moduleName = this.state.sqlTable.includeKey
      ? this.state.sqlTable.includeKey
      : undefined;
    sq.table.columnNames = this.sourceCols
      .filter((c) => c.checked)
      .map((c) => c.column.name);

    const filtersProto = createFiltersProto(
      this.state.filters,
      this.sourceCols,
    );
    if (filtersProto) sq.filters = filtersProto;
    const groupByProto = createGroupByProto(
      this.state.groupByColumns,
      this.state.aggregations,
    );
    if (groupByProto) sq.groupBy = groupByProto;

    const selectedColumns = createSelectColumnsProto(this);
    if (selectedColumns) sq.selectColumns = selectedColumns;
    return sq;
  }
}
