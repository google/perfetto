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
import {StructuredQueryBuilder} from '../../structured_query_builder';
import {ColumnInfo, columnInfoFromSqlColumn} from '../../column_info';
import protos from '../../../../../protos';
import {TextParagraph} from '../../../../../widgets/text_paragraph';
import {Trace} from '../../../../../public/trace';
import {closeModal, showModal} from '../../../../../widgets/modal';
import {TableList} from '../../table_list';
import {redrawModal} from '../../../../../widgets/modal';
import {perfettoSqlTypeToString} from '../../../../../trace_processor/perfetto_sql_type';
import {setValidationError} from '../../node_issues';

export interface TableSourceSerializedState {
  sqlTable?: string;
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
  }

  get type() {
    return NodeType.kTable;
  }

  clone(): QueryNode {
    const stateCopy: TableSourceState = {
      trace: this.state.trace,
      sqlModules: this.state.sqlModules,
      sqlTable: this.state.sqlTable,
      onchange: this.state.onchange,
    };
    return new TableSourceNode(stateCopy);
  }

  nodeSpecificModify(): m.Child {
    return undefined;
  }

  validate(): boolean {
    // Clear any previous errors at the start of validation
    if (this.state.issues) {
      this.state.issues.clear();
    }

    if (this.state.sqlTable === undefined) {
      setValidationError(this.state, 'No table selected');
      return false;
    }

    return true;
  }

  getTitle(): string {
    return `${this.state.sqlTable?.name}`;
  }

  getStructuredQuery(): protos.PerfettoSqlStructuredQuery | undefined {
    if (!this.validate()) return;
    if (!this.state.sqlTable) return;

    const columnNames = this.finalCols
      .filter((c) => c.checked)
      .map((c) => c.column.name);

    const sq = StructuredQueryBuilder.fromTable(
      this.state.sqlTable.name,
      this.state.sqlTable.includeKey || undefined,
      columnNames,
      this.nodeId,
    );

    const selectedColumns = createSelectColumnsProto(this);
    if (selectedColumns) sq.selectColumns = selectedColumns;
    return sq;
  }

  serializeState(): TableSourceSerializedState {
    return {
      sqlTable: this.state.sqlTable?.name,
      comment: this.state.comment,
    };
  }

  nodeInfo(): m.Children {
    if (this.state.sqlTable != null) {
      const table = this.state.sqlTable;
      return m(
        '.pf-stdlib-table-node',
        m(
          '.pf-details-box',
          m(TextParagraph, {text: table.description}),
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
                  m('td', perfettoSqlTypeToString(col.type)),
                  m('td', col.description),
                );
              }),
            ),
          ),
        ),
      );
    }
    return m(
      'div',
      m(
        'p',
        'Provides direct access to trace data tables like slices, processes, threads, counters, and more.',
      ),
      m(
        'p',
        'Select a table from the modal dialog to see its description and available columns.',
      ),
    );
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
