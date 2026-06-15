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
import type {
  SqlModules,
  SqlModule,
  SqlTable,
  SqlColumn,
  SqlFunction,
  SqlTableFunction,
  SqlMacro,
  SqlArgument,
} from './sql_modules';
import type {TableColumn} from '../../components/widgets/sql/table/table_column';
import type {SqlTableDefinition} from '../../components/widgets/sql/table/table_description';
import {
  parsePerfettoSqlTypeFromString,
  type PerfettoSqlType,
} from '../../trace_processor/perfetto_sql_type';

// SqlModules loaded from stdlib_docs.json, without a Trace object.

class SimpleSqlColumn implements SqlColumn {
  readonly name: string;
  readonly type?: PerfettoSqlType;
  readonly description: string;

  constructor(
    name: string,
    typeStr: string,
    description: string,
    tableName: string,
  ) {
    this.name = name;
    // One unrecognized type must not take down the whole schema load (which
    // would silently disable autocomplete for every table) — drop the type info
    // for that column instead.
    const parsed = parsePerfettoSqlTypeFromString({
      type: typeStr,
      table: tableName,
      column: name,
    });
    this.type = parsed.ok ? parsed.value : undefined;
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

  constructor(
    includeKey: string,
    tags: string[],
    tables: SqlTable[],
    functions: SqlFunction[],
    tableFunctions: SqlTableFunction[],
    macros: SqlMacro[],
  ) {
    this.includeKey = includeKey;
    this.tags = tags;
    this.tables = tables;
    this.functions = functions;
    this.tableFunctions = tableFunctions;
    this.macros = macros;
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

// Shape of stdlib_docs.json entries (parsed manually; see load()).
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

interface StdlibArg {
  name: string;
  type: string;
  desc: string;
}

interface StdlibFunction {
  name: string;
  desc: string;
  visibility?: string;
  args: StdlibArg[];
  return_type: string;
  return_desc: string;
}

interface StdlibTableFunction {
  name: string;
  desc: string;
  visibility?: string;
  args: StdlibArg[];
  cols: StdlibColumn[];
}

interface StdlibMacro {
  name: string;
  desc: string;
  visibility?: string;
  args: StdlibArg[];
  return_type: string;
}

interface StdlibModule {
  module_name: string;
  tags?: string[];
  data_objects: StdlibDataObject[];
  functions?: StdlibFunction[];
  table_functions?: StdlibTableFunction[];
  macros?: StdlibMacro[];
}

interface StdlibPackage {
  modules: StdlibModule[];
}

function mapArgs(args: StdlibArg[]): SqlArgument[] {
  return args.map((a) => ({name: a.name, description: a.desc, type: a.type}));
}

// Private/internal callables aren't meant to be used directly.
function isPublic(visibility?: string): boolean {
  return visibility === undefined || visibility === 'public';
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
        const functions: SqlFunction[] = (mod.functions ?? [])
          .filter((f) => isPublic(f.visibility))
          .map((f) => ({
            name: f.name,
            description: f.desc,
            args: mapArgs(f.args),
            returnType: f.return_type,
            returnDesc: f.return_desc,
          }));
        const tableFunctions: SqlTableFunction[] = (mod.table_functions ?? [])
          .filter((f) => isPublic(f.visibility))
          .map((f) => ({
            name: f.name,
            description: f.desc,
            args: mapArgs(f.args),
            returnCols: f.cols.map(
              (col) => new SimpleSqlColumn(col.name, col.type, col.desc, f.name),
            ),
          }));
        const macros: SqlMacro[] = (mod.macros ?? [])
          .filter((mac) => isPublic(mac.visibility))
          .map((mac) => ({
            name: mac.name,
            description: mac.desc,
            args: mapArgs(mac.args),
            returnType: mac.return_type,
          }));
        this.modules.push(
          new SimpleSqlModule(
            includeKey,
            mod.tags ?? [],
            tables,
            functions,
            tableFunctions,
            macros,
          ),
        );
      }
    }
  }

  // `modules` is immutable after construction, so flatten each list once and
  // reuse it — completion calls these on every keystroke.
  private _tables?: SqlTable[];
  private _tableNames?: string[];
  private _functions?: SqlFunction[];
  private _tableFunctions?: SqlTableFunction[];
  private _macros?: SqlMacro[];

  listTables(): SqlTable[] {
    return (this._tables ??= this.modules.flatMap((mod) => mod.tables));
  }

  listModules(): SqlModule[] {
    return this.modules;
  }

  listTablesNames(): string[] {
    return (this._tableNames ??= this.listTables().map((t) => t.name));
  }

  listFunctions(): SqlFunction[] {
    return (this._functions ??= this.modules.flatMap((mod) => mod.functions));
  }

  listTableFunctions(): SqlTableFunction[] {
    return (this._tableFunctions ??= this.modules.flatMap(
      (mod) => mod.tableFunctions,
    ));
  }

  listMacros(): SqlMacro[] {
    return (this._macros ??= this.modules.flatMap((mod) => mod.macros));
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
      // stdlib_docs.json lives in the parent dist directory.
      const resp = await fetch(assetSrc('../stdlib_docs.json'));
      if (!resp.ok) {
        throw new Error(`HTTP ${resp.status}: ${resp.statusText}`);
      }
      const json = await resp.json();
      // Parse manually: Zod's .parse() uses new Function(), which CSP forbids.
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
