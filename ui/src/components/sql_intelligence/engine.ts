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

// Shared, lazily-loaded syntaqlite engine for SQL diagnostics. syntaqlite is an
// in-browser WASM parser for PerfettoSQL (also used by dev.perfetto.QueryPage
// for "format SQL"). Its runDiagnostics(sql) returns structured unknown_table /
// unknown_column / unknown_function / function_arity / parse errors with
// offsets, which powers the editor's live, pre-run error underlines in BOTH the
// main UI and BigTrace.
//
// This module owns ALL contact with syntaqlite. It loads the engine + the
// PerfettoSQL dialect once and is fed the caller's schema via applySchema() so
// stdlib tables/columns aren't flagged as unknown. Everything degrades
// gracefully: if the WASM fails to load, engineDiagnostics() returns undefined
// and the editor simply shows no diagnostics.
//
// Note: syntaqlite also exposes runCompletions(), but it returns a flat,
// unscoped identifier dump (all schema tables/columns, no CTE awareness) —
// strictly less useful than the static catalog, which scopes columns to
// referenced tables and carries types/docs/signatures. So completion stays
// catalog-driven (see completion.ts); only diagnostics use the engine.

import {Engine, type DiagnosticDetail} from 'syntaqlite';
import {assetSrc} from '../../base/assets';
import type {SqlSchema} from './schema';

export interface EngineDiagnostic {
  // Document offsets (UTF-16 code units, which match byte offsets for the ASCII
  // that SQL is in practice).
  readonly from: number;
  readonly to: number;
  readonly severity: 'error' | 'warning' | 'info' | 'hint';
  readonly message: string;
  readonly help?: string;
  // Structured detail for machine consumption (drives the help rewrite).
  readonly detail: DiagnosticDetail;
}

// ---------------------------------------------------------------------------
// Engine lifecycle (singleton).
// ---------------------------------------------------------------------------

let enginePromise: Promise<Engine | undefined> | undefined;
let readyEngine: Engine | undefined;
// The exact schema object last fed to the engine. Re-applied whenever a
// different instance arrives — callers hand us a referentially-stable schema
// that only changes identity when its contents do (new modules, a new trace, or
// a CREATE [OR REPLACE] that adds/changes a session table). Keying on identity
// (not table count) catches same-count column changes.
let lastAppliedSchema: SqlSchema | undefined;
let schemaApplied = false;
let pendingSchema: SqlSchema | undefined;
const readyCallbacks: Array<() => void> = [];
const schemaCallbacks: Array<() => void> = [];

// Resolves to the loaded engine (or undefined if the WASM couldn't load).
export function getSqlEngine(): Promise<Engine | undefined> {
  if (enginePromise === undefined) {
    enginePromise = loadEngine();
  }
  return enginePromise;
}

// Synchronous accessor: the engine if it has finished loading, else undefined
// (kicking the load off in the background so it's ready soon).
export function peekSqlEngine(): Engine | undefined {
  if (enginePromise === undefined) void getSqlEngine();
  return readyEngine;
}

// Fired once the engine WASM is ready (or immediately if it already is).
export function onSqlEngineReady(cb: () => void): void {
  if (readyEngine) cb();
  else readyCallbacks.push(cb);
}

// Fired once a non-empty schema has been applied — immediately if one already
// has, otherwise on the first application. Lets the editor refresh its
// diagnostics the moment the schema arrives, without waiting for a keystroke.
// Fire-once (the callbacks are cleared after firing) so registrations from
// repeated editor mounts don't accumulate; later schema growth is picked up by
// the next diagnostics run anyway (it re-reads + re-applies the schema).
export function onSqlSchemaApplied(cb: () => void): void {
  if (schemaApplied) cb();
  else schemaCallbacks.push(cb);
}

export function engineHasSchema(): boolean {
  return schemaApplied;
}

async function loadEngine(): Promise<Engine | undefined> {
  try {
    const engine = new Engine({
      runtimeJsPath: assetSrc('assets/syntaqlite-runtime.js'),
      runtimeWasmPath: assetSrc('assets/syntaqlite-runtime.wasm'),
    });
    await engine.load();
    const binding = await engine.loadDialectFromUrl(
      assetSrc('assets/syntaqlite-perfetto.wasm'),
      'syntaqlite_perfetto_dialect_template',
    );
    engine.setDialectPointer(binding.ptr);
    readyEngine = engine;
    applySchemaToEngine(engine);
    for (const cb of readyCallbacks.splice(0)) cb();
    return engine;
  } catch (e) {
    // Non-fatal: diagnostics fall back to "none".
    console.warn(
      'syntaqlite engine failed to load; SQL diagnostics disabled',
      e,
    );
    return undefined;
  }
}

// Provide the schema the engine should validate against. Cheap no-op while the
// schema is unchanged; safe to call on every keystroke.
export function applySchema(schema: SqlSchema | undefined): void {
  pendingSchema = schema ?? undefined;
  if (readyEngine) applySchemaToEngine(readyEngine);
}

function applySchemaToEngine(engine: Engine): void {
  const schema = pendingSchema;
  if (!schema) return;
  if (schema === lastAppliedSchema) return; // unchanged instance — cheap no-op
  const tables = schema.listTables();
  if (tables.length === 0) return; // nothing useful yet; retry when populated
  lastAppliedSchema = schema;
  // Mirrors syntaqlite's SessionContextPayload ({tables:[{name,columns}],
  // views, functions}); functions are baked into the PerfettoSQL dialect so we
  // only declare tables + their columns.
  const payload = {
    tables: tables.map((t) => ({
      name: t.name,
      columns: t.columns.map((c) => c.name),
    })),
    views: [],
    functions: [],
  };
  try {
    engine.setSessionContext(JSON.stringify(payload));
    schemaApplied = true;
    for (const cb of schemaCallbacks.splice(0)) cb();
  } catch (e) {
    console.warn('failed to feed SQL schema to syntaqlite engine', e);
  }
}

// Structured diagnostics for the whole document. undefined => engine not ready.
export function engineDiagnostics(sql: string): EngineDiagnostic[] | undefined {
  const engine = peekSqlEngine();
  if (!engine) return undefined;
  const res = engine.runDiagnostics(sql);
  if (!res.ok) return undefined;
  return res.diagnostics.map((d) => ({
    from: d.startOffset,
    to: d.endOffset,
    severity: d.severity,
    message: d.message,
    help: d.help,
    detail: d.detail,
  }));
}
