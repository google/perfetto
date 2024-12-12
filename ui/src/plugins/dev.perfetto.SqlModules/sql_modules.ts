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
  LegacyTableColumn,
  LegacyTableColumnSet,
} from '../../components/widgets/sql/legacy_table/column';
import {SqlTableDescription} from '../../components/widgets/sql/legacy_table/table_description';
import {SimpleColumn} from '../../components/widgets/sql/table/table';

// Handles the access to all of the Perfetto SQL modules accessible to Trace
//  Processor.
export interface SqlModules {
  // Returns names of all tables/views between all loaded Perfetto SQL modules.
  listTables(): string[];

  // Returns Perfetto SQL table/view if it was loaded in one of the Perfetto
  // SQL module.
  getModuleForTable(tableName: string): SqlModule | undefined;
}

// Handles the access to a specific Perfetto SQL Package. Package consists of
// Perfetto SQL modules.
export interface SqlPackage {
  readonly name: string;
  readonly modules: SqlModule[];

  // Returns names of all tables/views in this package.
  listTables(): string[];

  // Returns sqlModule containing table with provided name.
  getModuleForTable(tableName: string): SqlModule | undefined;

  // Returns sqlTableDescription of the table with provided name.
  getSqlTableDescription(tableName: string): SqlTableDescription | undefined;
}

// Handles the access to a specific Perfetto SQL module.
export interface SqlModule {
  readonly includeKey: string;
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
  readonly description: string;
  readonly type: string;
  readonly columns: SqlColumn[];

  // Returns all columns as TableColumns.
  getTableColumns(): (LegacyTableColumn | LegacyTableColumnSet)[];
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
  readonly description: string;
  readonly type: string;

  // Translates this column to SimpleColumn.
  asSimpleColumn(tableName: string): SimpleColumn;
}

// The definition of Perfetto SQL argument. Can be used for functions, table
// functions or macros.
export interface SqlArgument {
  readonly name: string;
  readonly description: string;
  readonly type: string;
}
