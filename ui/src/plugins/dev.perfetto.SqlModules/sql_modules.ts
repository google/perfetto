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

import {TableColumn} from '../../components/widgets/sql/table/table_column';
import {SqlTableDefinition} from '../../components/widgets/sql/table/table_description';
import {PerfettoSqlType} from '../../trace_processor/perfetto_sql_type';

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

  // Returns true if the module is disabled due to missing data in the trace.
  isModuleDisabled(moduleName: string): boolean;

  // Returns the set of all disabled module names.
  getDisabledModules(): ReadonlySet<string>;
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

  // Returns sqlTableDefinition of the table with provided name.
  getSqlTableDefinition(tableName: string): SqlTableDefinition | undefined;
}

// Handles the access to a specific Perfetto SQL module.
export interface SqlModule {
  readonly includeKey: string;
  readonly tags: string[];
  readonly tables: SqlTable[];
  readonly functions: SqlFunction[];
  readonly tableFunctions: SqlTableFunction[];
  readonly macros: SqlMacro[];

  // Returns sqlTable with provided name.
  getTable(tableName: string): SqlTable | undefined;

  // Returns sqlTableDefinition of the table with provided name.
  getSqlTableDefinition(tableName: string): SqlTableDefinition | undefined;
}

// The definition of Perfetto SQL table/view.
export interface SqlTable {
  readonly name: string;
  readonly includeKey?: string;
  readonly description: string;
  readonly type: string;
  readonly importance?: 'high' | 'mid' | 'low';
  readonly columns: SqlColumn[];

  // Returns all columns as TableColumns.
  getTableColumns(): TableColumn[];
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
  readonly type?: PerfettoSqlType;
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
