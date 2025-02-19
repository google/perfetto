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
import {SqlTable} from '../../../dev.perfetto.SqlModules/sql_modules';
import {QueryNode, NodeType} from '../../query_node';
import {
  ColumnControllerRow,
  columnControllerRowFromSqlColumn,
} from '../column_controller';
import protos from '../../../../protos';
import {TextParagraph} from '../../../../widgets/text_paragraph';

export class StdlibTableNode implements QueryNode {
  readonly type: NodeType = NodeType.kStdlibTable;
  prevNode = undefined;
  nextNode?: QueryNode;
  finished: boolean = true;

  dataName: string;
  columns: ColumnControllerRow[];

  sqlTable: SqlTable;

  constructor(sqlTable: SqlTable) {
    this.dataName = sqlTable.name;
    this.columns = sqlTable.columns.map((c) =>
      columnControllerRowFromSqlColumn(c, true),
    );
    this.sqlTable = sqlTable;
  }

  getDetails(): m.Child {
    return m(TextParagraph, {
      text: `
    Table '${this.sqlTable.name}' from module 
    '${this.sqlTable.includeKey ?? 'prelude'}'.`,
    });
  }

  validate(): boolean {
    return true;
  }
  getTitle(): string {
    return `Table ${this.sqlTable.name}`;
  }
  getStructuredQuery(): protos.PerfettoSqlStructuredQuery | undefined {
    if (!this.validate()) return;

    const sq = new protos.PerfettoSqlStructuredQuery();
    sq.id = `table_source_${this.sqlTable.name}`;
    sq.table = new protos.PerfettoSqlStructuredQuery.Table();
    sq.table.tableName = this.sqlTable.name;
    sq.table.moduleName = this.sqlTable.includeKey
      ? this.sqlTable.includeKey
      : undefined;
    sq.table.columnNames = this.columns
      .filter((c) => c.checked)
      .map((c) => c.column.name);

    const selectedColumns: protos.PerfettoSqlStructuredQuery.SelectColumn[] =
      [];
    for (const c of this.columns.filter((c) => c.checked)) {
      const newC = new protos.PerfettoSqlStructuredQuery.SelectColumn();
      newC.columnName = c.column.name;
      if (c.alias) {
        newC.alias = c.alias;
      }
      selectedColumns.push(newC);
    }
    sq.selectColumns = selectedColumns;
    return sq;
  }
}
