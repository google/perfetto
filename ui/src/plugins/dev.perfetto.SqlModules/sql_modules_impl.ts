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

import {z} from 'zod';
import {
  SqlModules,
  SqlColumn,
  SqlFunction,
  SqlArgument,
  SqlMacro,
  SqlModule,
  SqlPackage,
  SqlTable,
  SqlTableFunction,
} from './sql_modules';
import {SqlTableDescription} from '../../components/widgets/sql/table/table_description';
import {TableColumn} from '../../components/widgets/sql/table/table_column';
import {Trace} from '../../public/trace';
import {
  parsePerfettoSqlTypeFromString,
  PerfettoSqlType,
} from '../../trace_processor/perfetto_sql_type';
import {unwrapResult} from '../../base/result';
import {createTableColumn} from '../../components/widgets/sql/table/create_column';

export class SqlModulesImpl implements SqlModules {
  readonly packages: SqlPackage[];

  constructor(trace: Trace, docs: SqlModulesDocsSchema) {
    this.packages = docs.map((json) => new StdlibPackageImpl(trace, json));
  }

  getTable(tableName: string): SqlTable | undefined {
    for (const p of this.packages) {
      const t = p.getTable(tableName);
      if (t !== undefined) {
        return t;
      }
    }
    return;
  }

  listTables(): SqlTable[] {
    return this.packages.flatMap((p) => p.listTables());
  }

  listTablesNames(): string[] {
    return this.packages.flatMap((p) => p.listTablesNames());
  }

  getModuleForTable(tableName: string): SqlModule | undefined {
    for (const stdlibPackage of this.packages) {
      const maybeTable = stdlibPackage.getModuleForTable(tableName);
      if (maybeTable) {
        return maybeTable;
      }
    }
    return undefined;
  }

  listModules(): SqlModule[] {
    return this.packages.flatMap((p) => p.modules);
  }
}

export class StdlibPackageImpl implements SqlPackage {
  readonly name: string;
  readonly modules: SqlModule[];

  constructor(trace: Trace, docs: DocsPackageSchemaType) {
    this.name = docs.name;
    this.modules = [];
    for (const moduleJson of docs.modules) {
      this.modules.push(new StdlibModuleImpl(trace, moduleJson));
    }
  }

  getTable(tableName: string): SqlTable | undefined {
    for (const module of this.modules) {
      for (const t of module.tables) {
        if (t.name == tableName) {
          return t;
        }
      }
    }
    return undefined;
  }

  listTables(): SqlTable[] {
    return this.modules.flatMap((module) => module.tables);
  }

  listTablesNames(): string[] {
    return this.listTables().map((t) => t.name);
  }

  getModuleForTable(tableName: string): SqlModule | undefined {
    for (const module of this.modules) {
      for (const t of module.tables) {
        if (t.name == tableName) {
          return module;
        }
      }
    }
    return undefined;
  }

  getSqlTableDescription(tableName: string): SqlTableDescription | undefined {
    for (const module of this.modules) {
      for (const t of module.tables) {
        if (t.name == tableName) {
          return module.getSqlTableDescription(tableName);
        }
      }
    }
    return undefined;
  }
}

export class StdlibModuleImpl implements SqlModule {
  readonly includeKey: string;
  readonly tags: string[];
  readonly tables: SqlTable[];
  readonly functions: SqlFunction[];
  readonly tableFunctions: SqlTableFunction[];
  readonly macros: SqlMacro[];

  constructor(trace: Trace, docs: DocsModuleSchemaType) {
    this.includeKey = docs.module_name;
    this.tags = docs.tags;

    const neededInclude = this.includeKey.startsWith('prelude')
      ? undefined
      : this.includeKey;
    this.tables = docs.data_objects.map(
      (json) => new SqlTableImpl(trace, json, neededInclude),
    );

    this.functions = docs.functions.map((json) => new StdlibFunctionImpl(json));
    this.tableFunctions = docs.table_functions.map(
      (json) => new StdlibTableFunctionImpl(json),
    );
    this.macros = docs.macros.map((json) => new StdlibMacroImpl(json));
  }

  getTable(tableName: string): SqlTable | undefined {
    for (const t of this.tables) {
      if (t.name == tableName) {
        return t;
      }
    }
    return undefined;
  }

  getSqlTableDescription(tableName: string): SqlTableDescription | undefined {
    const sqlTable = this.getTable(tableName);
    if (sqlTable === undefined) {
      return undefined;
    }
    return {
      imports: [this.includeKey],
      name: sqlTable.name,
      columns: sqlTable.getTableColumns(),
    };
  }
}

class StdlibMacroImpl implements SqlMacro {
  readonly name: string;
  readonly summaryDesc: string;
  readonly description: string;
  readonly args: SqlArgument[];
  readonly returnType: string;

  constructor(docs: DocsMacroSchemaType) {
    this.name = docs.name;
    this.summaryDesc = docs.summary_desc;
    this.description = docs.desc;
    this.returnType = docs.return_type;
    this.args = [];
    this.args = docs.args.map((json) => new StdlibFunctionArgImpl(json));
  }
}

class StdlibTableFunctionImpl implements SqlTableFunction {
  readonly name: string;
  readonly summaryDesc: string;
  readonly description: string;
  readonly args: SqlArgument[];
  readonly returnCols: SqlColumn[];

  constructor(docs: DocsTableFunctionSchemaType) {
    this.name = docs.name;
    this.summaryDesc = docs.summary_desc;
    this.description = docs.desc;
    this.args = docs.args.map((json) => new StdlibFunctionArgImpl(json));
    this.returnCols = docs.cols.map(
      (json) => new StdlibColumnImpl(json, this.name),
    );
  }
}

class StdlibFunctionImpl implements SqlFunction {
  readonly name: string;
  readonly summaryDesc: string;
  readonly description: string;
  readonly args: SqlArgument[];
  readonly returnType: string;
  readonly returnDesc: string;

  constructor(docs: DocsFunctionSchemaType) {
    this.name = docs.name;
    this.summaryDesc = docs.summary_desc;
    this.description = docs.desc;
    this.returnType = docs.return_type;
    this.returnDesc = docs.return_desc;
    this.args = docs.args.map((json) => new StdlibFunctionArgImpl(json));
  }
}

class SqlTableImpl implements SqlTable {
  name: string;
  includeKey?: string;
  description: string;
  type: string;
  columns: SqlColumn[];
  idColumn: SqlColumn | undefined;

  constructor(
    readonly trace: Trace,
    docs: DocsDataObjectSchemaType,
    includeKey: string | undefined,
  ) {
    this.name = docs.name;
    this.includeKey = includeKey;
    this.description = docs.desc;
    this.type = docs.type;
    this.columns = docs.cols.map(
      (json) => new StdlibColumnImpl(json, this.name),
    );
  }

  getTableColumns(): TableColumn[] {
    return this.columns.map((col) =>
      createTableColumn({
        trace: this.trace,
        column: col.name,
        type: col.type,
      }),
    );
  }
}

class StdlibColumnImpl implements SqlColumn {
  name: string;
  type: PerfettoSqlType;
  description: string;

  constructor(docs: DocsArgOrColSchemaType, tableName: string) {
    this.name = docs.name;
    this.type = unwrapResult(
      parsePerfettoSqlTypeFromString({
        type: docs.type,
        table: tableName,
        column: this.name,
      }),
    );
    this.description = docs.desc;
  }
}

class StdlibFunctionArgImpl implements SqlArgument {
  name: string;
  description: string;
  type: string;

  constructor(docs: DocsArgOrColSchemaType) {
    this.type = docs.type;
    this.description = docs.desc;
    this.name = docs.name;
  }
}

const ARG_OR_COL_SCHEMA = z.object({
  name: z.string(),
  type: z.string(),
  desc: z.string(),
  table: z.string().nullable(),
  column: z.string().nullable(),
});
type DocsArgOrColSchemaType = z.infer<typeof ARG_OR_COL_SCHEMA>;

const DATA_OBJECT_SCHEMA = z.object({
  name: z.string(),
  desc: z.string(),
  summary_desc: z.string(),
  type: z.string(),
  cols: z.array(ARG_OR_COL_SCHEMA),
});
type DocsDataObjectSchemaType = z.infer<typeof DATA_OBJECT_SCHEMA>;

const FUNCTION_SCHEMA = z.object({
  name: z.string(),
  desc: z.string(),
  summary_desc: z.string(),
  args: z.array(ARG_OR_COL_SCHEMA),
  return_type: z.string(),
  return_desc: z.string(),
});
type DocsFunctionSchemaType = z.infer<typeof FUNCTION_SCHEMA>;

const TABLE_FUNCTION_SCHEMA = z.object({
  name: z.string(),
  desc: z.string(),
  summary_desc: z.string(),
  args: z.array(ARG_OR_COL_SCHEMA),
  cols: z.array(ARG_OR_COL_SCHEMA),
});
type DocsTableFunctionSchemaType = z.infer<typeof TABLE_FUNCTION_SCHEMA>;

const MACRO_SCHEMA = z.object({
  name: z.string(),
  desc: z.string(),
  summary_desc: z.string(),
  return_desc: z.string(),
  return_type: z.string(),
  args: z.array(ARG_OR_COL_SCHEMA),
});
type DocsMacroSchemaType = z.infer<typeof MACRO_SCHEMA>;

const MODULE_SCHEMA = z.object({
  module_name: z.string(),
  tags: z.array(z.string()),
  data_objects: z.array(DATA_OBJECT_SCHEMA),
  functions: z.array(FUNCTION_SCHEMA),
  table_functions: z.array(TABLE_FUNCTION_SCHEMA),
  macros: z.array(MACRO_SCHEMA),
});
type DocsModuleSchemaType = z.infer<typeof MODULE_SCHEMA>;

const PACKAGE_SCHEMA = z.object({
  name: z.string(),
  modules: z.array(MODULE_SCHEMA),
});
type DocsPackageSchemaType = z.infer<typeof PACKAGE_SCHEMA>;

export const SQL_MODULES_DOCS_SCHEMA = z.array(PACKAGE_SCHEMA);
export type SqlModulesDocsSchema = z.infer<typeof SQL_MODULES_DOCS_SCHEMA>;
