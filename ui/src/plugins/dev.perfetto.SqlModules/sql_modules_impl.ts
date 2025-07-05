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
  SqlType,
  TableAndColumn,
  createTableColumnFromPerfettoSql,
} from './sql_modules';
import {SqlTableDescription} from '../../components/widgets/sql/table/table_description';
import {TableColumn} from '../../components/widgets/sql/table/table_column';

export class SqlModulesImpl implements SqlModules {
  readonly packages: SqlPackage[];

  constructor(docs: SqlModulesDocsSchema) {
    this.packages = docs.map((json) => new StdlibPackageImpl(json));
  }

  findAllTablesWithLinkedId(tableAndColumn: TableAndColumn): SqlTable[] {
    const linkedIdTables: SqlTable[] = [];
    for (const t of this.listTables()) {
      const allLinkedCols = t.linkedIdColumns;
      if (
        allLinkedCols.find(
          (c) =>
            c.type.tableAndColumn &&
            c.type.tableAndColumn.isEqual(tableAndColumn),
        )
      ) {
        linkedIdTables.push(t);
      }
    }
    return linkedIdTables;
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

  constructor(docs: DocsPackageSchemaType) {
    this.name = docs.name;
    this.modules = [];
    for (const moduleJson of docs.modules) {
      this.modules.push(new StdlibModuleImpl(moduleJson));
    }
  }

  getTable(tableName: string): SqlTable | undefined {
    for (const module of this.modules) {
      for (const dataObj of module.dataObjects) {
        if (dataObj.name == tableName) {
          return dataObj;
        }
      }
    }
    return undefined;
  }

  listTables(): SqlTable[] {
    return this.modules.flatMap((module) => module.dataObjects);
  }

  listTablesNames(): string[] {
    return this.listTables().map((t) => t.name);
  }

  getModuleForTable(tableName: string): SqlModule | undefined {
    for (const module of this.modules) {
      for (const dataObj of module.dataObjects) {
        if (dataObj.name == tableName) {
          return module;
        }
      }
    }
    return undefined;
  }

  getSqlTableDescription(tableName: string): SqlTableDescription | undefined {
    for (const module of this.modules) {
      for (const dataObj of module.dataObjects) {
        if (dataObj.name == tableName) {
          return module.getSqlTableDescription(tableName);
        }
      }
    }
    return undefined;
  }
}

export class StdlibModuleImpl implements SqlModule {
  readonly includeKey: string;
  readonly tables: SqlTable[];
  readonly dataObjects: SqlTable[];
  readonly functions: SqlFunction[];
  readonly tableFunctions: SqlTableFunction[];
  readonly macros: SqlMacro[];

  constructor(docs: DocsModuleSchemaType) {
    this.includeKey = docs.module_name;

    const neededInclude = this.includeKey.startsWith('prelude')
      ? undefined
      : this.includeKey;
    this.tables = docs.data_objects.map(
      (json) => new SqlTableImpl(json, neededInclude),
    );
    this.dataObjects = this.tables;

    this.functions = docs.functions.map((json) => new StdlibFunctionImpl(json));
    this.tableFunctions = docs.table_functions.map(
      (json) => new StdlibTableFunctionImpl(json),
    );
    this.macros = docs.macros.map((json) => new StdlibMacroImpl(json));
  }

  getTable(tableName: string): SqlTable | undefined {
    for (const obj of this.dataObjects) {
      if (obj.name == tableName) {
        return obj;
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
    this.returnCols = docs.cols.map((json) => new StdlibColumnImpl(json));
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
  linkedIdColumns: SqlColumn[];
  joinIdColumns: SqlColumn[];

  constructor(docs: DocsDataObjectSchemaType, includeKey: string | undefined) {
    this.name = docs.name;
    this.includeKey = includeKey;
    this.description = docs.desc;
    this.type = docs.type;
    this.columns = docs.cols.map((json) => new StdlibColumnImpl(json));

    this.linkedIdColumns = [];
    this.joinIdColumns = [];
    for (const c of this.columns) {
      if (c.type.name === 'id') {
        this.idColumn = c;
        continue;
      }
      if (c.type.shortName === 'id') {
        this.linkedIdColumns.push(c);
        continue;
      }
      if (c.type.shortName === 'joinid') {
        this.joinIdColumns.push(c);
        continue;
      }
    }
  }

  getIdColumns(): SqlColumn[] {
    return this.columns.filter((c) => c.type.shortName === 'id');
  }

  getJoinIdColumns(): SqlColumn[] {
    return this.columns.filter((c) => c.type.shortName === 'joinid');
  }

  getIdTables(): TableAndColumn[] {
    return this.getIdColumns()
      .map((c) => c.type.tableAndColumn)
      .filter((tAndC) => tAndC !== undefined) as TableAndColumn[];
  }

  getJoinIdTables(): TableAndColumn[] {
    return this.getJoinIdColumns()
      .map((c) => c.type.tableAndColumn)
      .filter((tAndC) => tAndC !== undefined) as TableAndColumn[];
  }

  getTableColumns(): TableColumn[] {
    return this.columns.map((col) =>
      createTableColumnFromPerfettoSql(col, this.name),
    );
  }
}

class StdlibColumnImpl implements SqlColumn {
  name: string;
  type: SqlType;
  description: string;

  constructor(docs: DocsArgOrColSchemaType) {
    this.type = {
      name: docs.type.toLowerCase(),
      shortName: docs.type.split('(')[0].toLowerCase(),
      tableAndColumn:
        docs.table && docs.column
          ? new TableAndColumnImpl(
              docs.table.toLowerCase(),
              docs.column.toLowerCase(),
            )
          : undefined,
    };
    this.description = docs.desc;
    this.name = docs.name;
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

export class TableAndColumnImpl implements TableAndColumn {
  table: string;
  column: string;
  constructor(table: string, column: string) {
    this.table = table;
    this.column = column;
  }
  isEqual(o: TableAndColumn): boolean {
    return o.table === this.table && o.column === this.column;
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
