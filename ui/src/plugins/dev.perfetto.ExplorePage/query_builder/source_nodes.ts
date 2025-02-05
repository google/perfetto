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

import {TableAndColumnImpl} from '../../dev.perfetto.SqlModules/sql_modules_impl';
import {SqlTable, SqlColumn} from '../../dev.perfetto.SqlModules/sql_modules';
import {QueryNode, NodeType} from '../query_state';
import {
  ColumnControllerRow,
  columnControllerRowFromName,
  columnControllerRowFromSqlColumn,
} from './column_controller';

import protos from '../../../protos';

export class StdlibTableState implements QueryNode {
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
      columnControllerRowFromSqlColumn(c),
    );
    this.sqlTable = sqlTable;
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

export interface SimpleSlicesAttrs {
  slice_name?: string;
  thread_name?: string;
  process_name?: string;
  track_name?: string;
}

export class SimpleSlicesState implements QueryNode {
  type: NodeType = NodeType.kSimpleSlices;
  prevNode = undefined;
  nextNode?: QueryNode;
  finished: boolean = true;

  dataName: string = 'Simple slices';
  columns: ColumnControllerRow[];

  attrs: SimpleSlicesAttrs;

  validate(): boolean {
    return (
      this.attrs.slice_name !== undefined ||
      this.attrs.process_name !== undefined ||
      this.attrs.thread_name !== undefined ||
      this.attrs.track_name !== undefined
    );
  }
  getTitle(): string {
    return `Simple slices`;
  }
  getStructuredQuery(): protos.PerfettoSqlStructuredQuery | undefined {
    if (!this.validate()) return;

    const sq = new protos.PerfettoSqlStructuredQuery();
    sq.id = `simple_slices_source`;
    const ss = new protos.PerfettoSqlStructuredQuery.SimpleSlices();

    if (this.attrs.slice_name) ss.sliceNameGlob = this.attrs.slice_name;
    if (this.attrs.thread_name) ss.threadNameGlob = this.attrs.thread_name;
    if (this.attrs.process_name) ss.processNameGlob = this.attrs.process_name;
    if (this.attrs.track_name) ss.trackNameGlob = this.attrs.track_name;

    sq.simpleSlices = ss;

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

  constructor(attrs: SimpleSlicesAttrs) {
    this.attrs = attrs;
    const cols: SqlColumn[] = [
      {
        name: 'id',
        type: {
          name: 'ID(slice.id)',
          shortName: 'id',
          tableAndColumn: new TableAndColumnImpl('string', 'id'),
        },
      },
      {
        name: 'ts',
        type: {
          name: 'TIMESTAMP',
          shortName: 'TIMESTAMP',
        },
      },
      {
        name: 'dur',
        type: {
          name: 'DURATION',
          shortName: 'DURATION',
        },
      },
      {
        name: 'slice_name',
        type: {
          name: 'STRING',
          shortName: 'STRING',
        },
      },
      {
        name: 'thread_name',
        type: {
          name: 'STRING',
          shortName: 'STRING',
        },
      },
      {
        name: 'process_name',
        type: {
          name: 'STRING',
          shortName: 'STRING',
        },
      },
      {
        name: 'track_name',
        type: {
          name: 'STRING',
          shortName: 'STRING',
        },
      },
    ];
    this.columns = cols.map((c) => columnControllerRowFromSqlColumn(c));
  }
}

export interface SqlSourceAttrs {
  sql?: string;
  columns?: string[];
  preamble?: string;
}

export class SqlSourceState implements QueryNode {
  type: NodeType = NodeType.kSqlSource;
  prevNode = undefined;
  nextNode?: QueryNode;
  finished = true;

  dataName: string = 'Sql source';
  columns: ColumnControllerRow[];

  attrs: SqlSourceAttrs;

  validate(): boolean {
    return (
      this.attrs.sql !== undefined &&
      this.attrs.columns !== undefined &&
      this.attrs.preamble !== undefined &&
      this.columns.length > 0
    );
  }
  getTitle(): string {
    return `Sql source`;
  }
  getStructuredQuery(): protos.PerfettoSqlStructuredQuery | undefined {
    if (!this.validate()) return;

    const sq = new protos.PerfettoSqlStructuredQuery();
    sq.id = `sql_source`;
    const sqlProto = new protos.PerfettoSqlStructuredQuery.Sql();

    if (this.attrs.sql) sqlProto.sql = this.attrs.sql;
    if (this.attrs.columns) sqlProto.columnNames = this.attrs.columns;
    if (this.attrs.preamble) sqlProto.preamble = this.attrs.preamble;

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
    sq.sql = sqlProto;
    sq.selectColumns = selectedColumns;
    return sq;
  }

  constructor(attrs: SqlSourceAttrs) {
    this.attrs = attrs;
    this.columns =
      attrs.columns?.map((c) => columnControllerRowFromName(c)) ?? [];
  }
}
