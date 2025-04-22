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
  NodeType,
  createSelectColumnsProto,
  QueryNodeState,
  createFinalColumns,
} from '../../query_node';
import {
  ColumnControllerRow,
  columnControllerRowFromSqlColumn,
  newColumnControllerRows,
} from '../column_controller';
import protos from '../../../../protos';
import {TextParagraph} from '../../../../widgets/text_paragraph';
import {Trace} from '../../../../public/trace';
import {Button, ButtonVariant} from '../../../../widgets/button';
import {closeModal} from '../../../../widgets/modal';
import {
  createFiltersProto,
  createGroupByProto,
  Operator,
} from '../operations/operation_component';
import {Intent} from '../../../../widgets/common';

export interface StdlibTableAttrs extends QueryNodeState {
  readonly trace: Trace;
  readonly sqlModules: SqlModules;

  sqlTable?: SqlTable;
}

export class StdlibTableNode implements QueryNode {
  readonly type: NodeType = NodeType.kStdlibTable;
  readonly prevNode = undefined;
  nextNode?: QueryNode;

  readonly sourceCols: ColumnControllerRow[];
  readonly finalCols: ColumnControllerRow[];
  readonly state: StdlibTableAttrs;

  constructor(attrs: StdlibTableAttrs) {
    this.state = attrs;

    this.sourceCols = attrs.sourceCols ?? [];
    this.finalCols = createFinalColumns(this);
  }

  getStateCopy(): QueryNodeState {
    const newState: StdlibTableAttrs = {
      trace: this.state.trace,
      sqlModules: this.state.sqlModules,
      sqlTable: this.state.sqlTable,
      sourceCols: newColumnControllerRows(this.sourceCols),
      groupByColumns: newColumnControllerRows(this.state.groupByColumns),
      filters: this.state.filters.map((f) => ({...f})),
      aggregations: this.state.aggregations.map((a) => ({...a})),
    };
    return newState;
  }

  getDetails(): m.Child {
    return m(
      '',
      m(Button, {
        label: this.state.sqlTable ? 'Change table' : 'Select table',
        intent: Intent.Primary,
        variant: ButtonVariant.Filled,
        onclick: async () => {
          const tableName = await this.state.trace.omnibox.prompt(
            'Choose a table...',
            this.state.sqlModules.listTablesNames(),
          );

          if (!tableName) return;
          const sqlTable = this.state.sqlModules.getTable(tableName);

          if (!sqlTable) return;
          this.state.sqlTable = sqlTable;
          this.state.sourceCols = sqlTable.columns.map((c) =>
            columnControllerRowFromSqlColumn(c, true),
          );
          this.state.filters = [];
          this.state.groupByColumns = newColumnControllerRows(
            this.state.sourceCols,
            false,
          );
        },
      }),
    );
  }

  validate(): boolean {
    return this.state.sqlTable !== undefined;
  }

  getTitle(): string {
    return `Table ${this.state.sqlTable?.name}`;
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

    const filtersProto = createFiltersProto(this.state.filters);
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

export class StdlibTableSource implements m.ClassComponent<StdlibTableAttrs> {
  async onTableSelect(attrs: StdlibTableAttrs) {
    closeModal();
    const tableName = await attrs.trace.omnibox.prompt(
      'Choose a table...',
      attrs.sqlModules.listTablesNames(),
    );

    if (!tableName) return;
    const sqlTable = attrs.sqlModules.getTable(tableName);

    if (!sqlTable) return;
    attrs.sqlTable = sqlTable;
    attrs.sourceCols = sqlTable.columns.map((c) =>
      columnControllerRowFromSqlColumn(c, true),
    );
    attrs.filters = [];
    attrs.groupByColumns = newColumnControllerRows(attrs.sourceCols, false);
  }

  view({attrs}: m.CVnode<StdlibTableAttrs>) {
    const tableInfoStr = attrs.sqlTable
      ? `Selected table: ${attrs.sqlTable.name}`
      : 'No table selected';
    const tableInfo = m(TextParagraph, {text: tableInfoStr});

    return m(
      '',
      m(Button, {
        label: attrs.sqlTable ? 'Change table' : 'Select table',
        intent: Intent.Primary,
        variant: ButtonVariant.Filled,
        onclick: async () => {
          this.onTableSelect(attrs);
        },
      }),
      attrs.sqlTable && [
        tableInfo,
        m(Operator, {
          filter: {sourceCols: attrs.sourceCols, filters: attrs.filters},
          groupby: {
            groupByColumns: attrs.groupByColumns,
            aggregations: attrs.aggregations,
          },
        }),
      ],
    );
  }
}
