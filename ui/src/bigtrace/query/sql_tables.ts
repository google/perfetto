// Copyright (C) 2026 The Android Open Source Project
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
import {assetSrc} from '../../base/assets';
import {
  SqlModules,
  SqlModule,
  SqlTable,
  SqlColumn,
  SqlFunction,
  SqlTableFunction,
  SqlMacro,
} from '../../plugins/dev.perfetto.SqlModules/sql_modules';
import {TableColumn} from '../../components/widgets/sql/table/table_column';
import {SqlTableDefinition} from '../../components/widgets/sql/table/table_description';
import {
  parsePerfettoSqlTypeFromString,
  PerfettoSqlType,
} from '../../trace_processor/perfetto_sql_type';
import {unwrapResult} from '../../base/result';

// Lightweight SqlModules implementation for BigTrace that loads stdlib_docs.json
// without requiring a Trace object.

class SimpleSqlColumn implements SqlColumn {
  readonly name: string;
  readonly type: PerfettoSqlType;
  readonly description: string;

  constructor(
    name: string,
    typeStr: string,
    description: string,
    tableName: string,
  ) {
    this.name = name;
    this.type = unwrapResult(
      parsePerfettoSqlTypeFromString({
        type: typeStr,
        table: tableName,
        column: name,
      }),
    );
    this.description = description;
  }
}

class SimpleSqlTable implements SqlTable {
  readonly name: string;
  readonly includeKey?: string;
  readonly description: string;
  readonly type: string;
  readonly importance?: 'core' | 'high' | 'mid' | 'low';
  readonly columns: SqlColumn[];

  constructor(
    name: string,
    description: string,
    type: string,
    columns: SqlColumn[],
    includeKey?: string,
    importance?: 'core' | 'high' | 'mid' | 'low',
  ) {
    this.name = name;
    this.description = description;
    this.type = type;
    this.columns = columns;
    this.includeKey = includeKey;
    this.importance = importance;
  }

  getTableColumns(): TableColumn[] {
    return [];
  }
}

class SimpleSqlModule implements SqlModule {
  readonly includeKey: string;
  readonly tags: string[];
  readonly tables: SqlTable[];
  readonly functions: SqlFunction[];
  readonly tableFunctions: SqlTableFunction[];
  readonly macros: SqlMacro[];

  constructor(includeKey: string, tags: string[], tables: SqlTable[]) {
    this.includeKey = includeKey;
    this.tags = tags;
    this.tables = tables;
    this.functions = [];
    this.tableFunctions = [];
    this.macros = [];
  }

  getTable(tableName: string): SqlTable | undefined {
    return this.tables.find((t) => t.name === tableName);
  }

  getSqlTableDefinition(tableName: string): SqlTableDefinition | undefined {
    const table = this.getTable(tableName);
    if (!table) return undefined;
    return {
      imports: [this.includeKey],
      name: table.name,
      columns: table.columns.map((col) => ({
        column: col.name,
        type: col.type,
      })),
    };
  }
}

// Shape of the stdlib_docs.json entries (parsed without Zod to avoid CSP).
interface StdlibColumn {
  name: string;
  type: string;
  desc: string;
}

interface StdlibDataObject {
  name: string;
  desc: string;
  type: string;
  cols: StdlibColumn[];
  importance?: 'core' | 'high' | 'mid' | 'low';
}

interface StdlibModule {
  module_name: string;
  tags?: string[];
  data_objects: StdlibDataObject[];
}

interface StdlibPackage {
  modules: StdlibModule[];
}

class SimpleSqlModules implements SqlModules {
  private modules: SimpleSqlModule[];

  constructor(docs: unknown[]) {
    this.modules = [];
    for (const pkg of docs as StdlibPackage[]) {
      for (const mod of pkg.modules) {
        const includeKey: string = mod.module_name;
        const neededInclude = includeKey.startsWith('prelude')
          ? undefined
          : includeKey;
        const tables = mod.data_objects.map((obj) => {
          const columns = obj.cols.map(
            (col) =>
              new SimpleSqlColumn(col.name, col.type, col.desc, obj.name),
          );
          return new SimpleSqlTable(
            obj.name,
            obj.desc,
            obj.type,
            columns,
            neededInclude,
            obj.importance ?? undefined,
          );
        });
        this.modules.push(
          new SimpleSqlModule(includeKey, mod.tags ?? [], tables),
        );
      }
    }
  }

  listTables(): SqlTable[] {
    return this.modules.flatMap((mod) => mod.tables);
  }

  listModules(): SqlModule[] {
    return this.modules;
  }

  listTablesNames(): string[] {
    return this.listTables().map((t) => t.name);
  }

  getTable(tableName: string): SqlTable | undefined {
    for (const mod of this.modules) {
      const t = mod.getTable(tableName);
      if (t) return t;
    }
    return undefined;
  }

  getModuleForTable(tableName: string): SqlModule | undefined {
    for (const mod of this.modules) {
      if (mod.tables.some((t) => t.name === tableName)) {
        return mod;
      }
    }
    return undefined;
  }

  isModuleDisabled(_moduleName: string): boolean {
    return false;
  }

  getDisabledModules(): ReadonlySet<string> {
    return new Set();
  }

  ensureInitialized(): Promise<void> {
    return Promise.resolve();
  }
}

// Singleton that lazily loads the stdlib docs.
class SqlTablesLoader {
  private sqlModules: SqlModules | undefined;
  private loading = false;
  private error: string | undefined;

  get modules(): SqlModules | undefined {
    return this.sqlModules;
  }

  get isLoading(): boolean {
    return this.loading;
  }

  get loadError(): string | undefined {
    return this.error;
  }

  async load(): Promise<void> {
    if (this.sqlModules || this.loading) return;
    this.loading = true;
    this.error = undefined;
    try {
      // stdlib_docs.json lives in the parent Perfetto dist directory.
      const resp = await fetch(assetSrc('../stdlib_docs.json'));
      if (!resp.ok) {
        throw new Error(`HTTP ${resp.status}: ${resp.statusText}`);
      }
      const json = await resp.json();
      // Avoid Zod .parse() here — it uses new Function() which violates CSP.
      this.sqlModules = new SimpleSqlModules(json);
    } catch (e) {
      this.error = e instanceof Error ? e.message : String(e);
    } finally {
      this.loading = false;
      m.redraw();
    }
  }
}

export const sqlTablesLoader = new SqlTablesLoader();
