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

// All Perfetto SQL modules accessible to Trace Processor.
export interface SqlModules {
  listTables(): SqlTable[];
  listModules(): SqlModule[];
  listTablesNames(): string[];
  getTable(tableName: string): SqlTable | undefined;
  getModuleForTable(tableName: string): SqlModule | undefined;
  isModuleDisabled(moduleName: string): boolean;

  // undefined if no per-table check exists; callers fall back to isModuleDisabled.
  tablePassedDataCheck?(tableName: string): boolean | undefined;

  getDisabledModules(): ReadonlySet<string>;

  // Idempotent; resolves once availability checks complete.
  ensureInitialized(): Promise<void>;
}

// A Perfetto SQL package (a set of modules).
export interface SqlPackage {
  readonly name: string;
  readonly modules: SqlModule[];

  listTables(): SqlTable[];
  listTablesNames(): string[];
  getTable(tableName: string): SqlTable | undefined;
  getModuleForTable(tableName: string): SqlModule | undefined;
  getSqlTableDefinition(tableName: string): SqlTableDefinition | undefined;
}

// A single Perfetto SQL module.
export interface SqlModule {
  readonly includeKey: string;
  readonly tags: string[];
  readonly tables: SqlTable[];
  readonly functions: SqlFunction[];
  readonly tableFunctions: SqlTableFunction[];
  readonly macros: SqlMacro[];

  getTable(tableName: string): SqlTable | undefined;
  getSqlTableDefinition(tableName: string): SqlTableDefinition | undefined;
}

export interface SqlTable {
  readonly name: string;
  readonly includeKey?: string;
  readonly description: string;
  readonly type: string;
  readonly importance?: 'core' | 'high' | 'mid' | 'low';
  readonly dataCheckSql?: string;
  readonly columns: SqlColumn[];

  getTableColumns(): TableColumn[];
}

export interface SqlFunction {
  readonly name: string;
  readonly description: string;
  readonly args: SqlArgument[];
  readonly returnType: string;
  readonly returnDesc: string;
}

export interface SqlTableFunction {
  readonly name: string;
  readonly description: string;
  readonly args: SqlArgument[];
  readonly returnCols: SqlColumn[];
}

export interface SqlMacro {
  readonly name: string;
  readonly description: string;
  readonly args: SqlArgument[];
  readonly returnType: string;
}

export interface SqlColumn {
  readonly name: string;
  readonly description?: string;
  readonly type?: PerfettoSqlType;
}

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

// Per-table availability if known; else falls back to module-level.
export function isTableEffectivelyDisabled(
  sqlModules: SqlModules,
  tableName: string,
): boolean {
  const availability = sqlModules.tablePassedDataCheck?.(tableName);
  if (availability !== undefined) {
    return !availability;
  }
  const module = sqlModules.getModuleForTable(tableName);
  return module !== undefined && sqlModules.isModuleDisabled(module.includeKey);
}
