// Copyright (C) 2024 The Android Open Source Project
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

import {
  DurationColumn,
  ProcessIdColumn,
  SchedIdColumn,
  SliceIdColumn,
  StandardColumn,
  ThreadIdColumn,
  ThreadStateIdColumn,
  TimestampColumn,
} from '../../components/widgets/sql/table/columns';
import {TableColumn} from '../../components/widgets/sql/table/table_column';
import {SqlTableDescription} from '../../components/widgets/sql/table/table_description';

// Handles the access to all of the Perfetto SQL modules accessible to Trace
//  Processor.
export interface SqlModules {
  // Returns all tables/views between all loaded Perfetto SQL modules.
  listTables(): SqlTable[];

  // Returns all loaded Perfetto SQL modules.
  listModules(): SqlModule[];

  // Returns names of all tables/views between all loaded Perfetto SQL modules.
  listTablesNames(): string[];

  // Returns Perfetto SQL table/view if it was loaded in one of the Perfetto
  // SQL module.
  getTable(tableName: string): SqlTable | undefined;

  // Returns module that contains Perfetto SQL table/view if it was loaded in one of the Perfetto
  // SQL module.
  getModuleForTable(tableName: string): SqlModule | undefined;

  findAllTablesWithLinkedId(tableAndColumn: TableAndColumn): SqlTable[];
}

// Handles the access to a specific Perfetto SQL Package. Package consists of
// Perfetto SQL modules.
export interface SqlPackage {
  readonly name: string;
  readonly modules: SqlModule[];

  // Returns all tables/views in this package.
  listTables(): SqlTable[];

  // Returns names of all tables/views in this package.
  listTablesNames(): string[];

  getTable(tableName: string): SqlTable | undefined;

  // Returns sqlModule containing table with provided name.
  getModuleForTable(tableName: string): SqlModule | undefined;

  // Returns sqlTableDescription of the table with provided name.
  getSqlTableDescription(tableName: string): SqlTableDescription | undefined;
}

// Handles the access to a specific Perfetto SQL module.
export interface SqlModule {
  readonly includeKey: string;
  readonly tables: SqlTable[];
  readonly dataObjects: SqlTable[];
  readonly functions: SqlFunction[];
  readonly tableFunctions: SqlTableFunction[];
  readonly macros: SqlMacro[];

  // Returns sqlTable with provided name.
  getTable(tableName: string): SqlTable | undefined;

  // Returns sqlTableDescription of the table with provided name.
  getSqlTableDescription(tableName: string): SqlTableDescription | undefined;
}

// The definition of Perfetto SQL table/view.
export interface SqlTable {
  readonly name: string;
  readonly includeKey?: string;
  readonly description: string;
  readonly type: string;
  readonly columns: SqlColumn[];

  readonly idColumn: SqlColumn | undefined;
  readonly linkedIdColumns: SqlColumn[];
  readonly joinIdColumns: SqlColumn[];

  // Returns all columns as TableColumns.
  getTableColumns(): TableColumn[];

  getIdColumns(): SqlColumn[];
  getJoinIdColumns(): SqlColumn[];

  getIdTables(): TableAndColumn[];
  getJoinIdTables(): TableAndColumn[];
}

// The definition of Perfetto SQL function.
export interface SqlFunction {
  readonly name: string;
  readonly description: string;
  readonly args: SqlArgument[];
  readonly returnType: string;
  readonly returnDesc: string;
}

// The definition of Perfetto SQL table function.
export interface SqlTableFunction {
  readonly name: string;
  readonly description: string;
  readonly args: SqlArgument[];
  readonly returnCols: SqlColumn[];
}

// The definition of Perfetto SQL macro.
export interface SqlMacro {
  readonly name: string;
  readonly description: string;
  readonly args: SqlArgument[];
  readonly returnType: string;
}

// The definition of Perfetto SQL column.
export interface SqlColumn {
  readonly name: string;
  readonly description?: string;
  readonly type: SqlType;
}

// The definition of Perfetto SQL argument. Can be used for functions, table
// functions or macros.
export interface SqlArgument {
  readonly name: string;
  readonly description: string;
  readonly type: string;
}

export interface TableAndColumn {
  table: string;
  column: string;

  isEqual(o: TableAndColumn): boolean;
}

export interface SqlType {
  readonly name: string;
  readonly shortName: string;
  readonly tableAndColumn?: TableAndColumn;
}

export function createTableColumnFromPerfettoSql(
  col: SqlColumn,
  tableName: string,
): TableColumn {
  if (col.type.shortName === 'timestamp') {
    return new TimestampColumn(col.name);
  }
  if (col.type.shortName === 'duration') {
    return new DurationColumn(col.name);
  }

  if (col.type.shortName === 'id') {
    switch (tableName.toLowerCase()) {
      case 'slice':
        return new SliceIdColumn(col.name, {type: 'id'});
      case 'thread':
        return new ThreadIdColumn(col.name, {type: 'id'});
      case 'process':
        return new ProcessIdColumn(col.name, {type: 'id'});
      case 'thread_state':
        return new ThreadStateIdColumn(col.name);
      case 'sched':
        return new SchedIdColumn(col.name);
    }
    return new StandardColumn(col.name);
  }

  if (col.type.shortName === 'joinid') {
    if (col.type.tableAndColumn === undefined) {
      return new StandardColumn(col.name);
    }
    switch (col.type.tableAndColumn.table.toLowerCase()) {
      case 'slice':
        return new SliceIdColumn(col.name);
      case 'thread':
        return new ThreadIdColumn(col.name);
      case 'process':
        return new ProcessIdColumn(col.name);
      case 'thread_state':
        return new ThreadStateIdColumn(col.name);
      case 'sched':
        return new SchedIdColumn(col.name);
    }
  }

  return new StandardColumn(col.name);
}
