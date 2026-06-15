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

// The minimal schema view the SQL-intelligence layer needs. It is a structural
// SUBSET of the `SqlModules` interface used by both the main UI
// (dev.perfetto.SqlModules) and the BigTrace UI (bigtrace/query/sql_modules) —
// both of those satisfy this without any adapter, which is how completion +
// diagnostics are shared verbatim across the two bundles. We define it here
// (rather than importing one of the two SqlModules) so this lives in the
// components layer and depends on nothing app-specific (the strict layering
// forbids components from importing plugins, and BigTrace can't see plugins
// either).

import type {PerfettoSqlType} from '../../trace_processor/perfetto_sql_type';

export interface SqlSchemaColumn {
  readonly name: string;
  readonly description?: string;
  readonly type?: PerfettoSqlType;
}

export interface SqlSchemaTable {
  readonly name: string;
  // undefined for prelude / built-in tables that need no INCLUDE.
  readonly includeKey?: string;
  readonly description: string;
  readonly columns: SqlSchemaColumn[];
}

export interface SqlSchemaArg {
  readonly name: string;
  readonly type: string;
}

// A scalar function or a macro: `name(args) -> returnType`.
export interface SqlSchemaCallable {
  readonly name: string;
  readonly description: string;
  readonly args: SqlSchemaArg[];
  readonly returnType: string;
}

// A table-valued function: `name(args) -> TABLE`.
export interface SqlSchemaTableFunction {
  readonly name: string;
  readonly description: string;
  readonly args: SqlSchemaArg[];
}

export interface SqlSchemaModule {
  readonly includeKey: string;
  readonly functions: SqlSchemaCallable[];
  readonly tableFunctions: SqlSchemaTableFunction[];
  readonly macros: SqlSchemaCallable[];
}

export interface SqlSchema {
  listTables(): SqlSchemaTable[];
  listTablesNames(): string[];
  getTable(name: string): SqlSchemaTable | undefined;
  listModules(): SqlSchemaModule[];
}

// A late-bound getter for the schema: it streams in asynchronously (catalog
// fetch / per-trace availability checks), so completion + diagnostics read it
// fresh on each invocation and improve as it loads.
export type SqlSchemaProvider = () => SqlSchema | undefined;

// ---------------------------------------------------------------------------
// Flatten a schema's callables out of its modules (memoized per schema
// instance). Lets completion offer functions/macros/table-functions using only
// the common `listModules()` method — no `listFunctions()` etc. on the
// interface, so the main UI's SqlModules satisfies it unchanged.
// ---------------------------------------------------------------------------

interface FlatCallables {
  readonly functions: SqlSchemaCallable[];
  readonly tableFunctions: SqlSchemaTableFunction[];
  readonly macros: SqlSchemaCallable[];
}

const callableCache = new WeakMap<SqlSchema, FlatCallables>();

export function flattenCallables(schema: SqlSchema): FlatCallables {
  const cached = callableCache.get(schema);
  if (cached) return cached;
  const functions: SqlSchemaCallable[] = [];
  const tableFunctions: SqlSchemaTableFunction[] = [];
  const macros: SqlSchemaCallable[] = [];
  for (const mod of schema.listModules()) {
    functions.push(...mod.functions);
    tableFunctions.push(...mod.tableFunctions);
    macros.push(...mod.macros);
  }
  const flat = {functions, tableFunctions, macros};
  callableCache.set(schema, flat);
  return flat;
}
