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

import {beforeEach, describe, expect, test, vi} from 'vitest';

// Mock the engine so we can drive the raw diagnostics + schema-applied state
// without a real WASM engine.
vi.mock('./engine', () => ({
  applySchema: vi.fn(),
  engineDiagnostics: vi.fn(),
  engineHasSchema: vi.fn(),
}));

import {applySchema, engineDiagnostics, engineHasSchema} from './engine';
import type {EngineDiagnostic} from './engine';
import {createPerfettoSqlDiagnosticsSource} from './diagnostics';
import type {SqlSchema, SqlSchemaTable} from './schema';

const STARTUPS: SqlSchemaTable = {
  name: 'android_startups',
  includeKey: 'android.startup.startups',
  description: 'Startups',
  columns: [{name: 'startup_id'}],
};

function fakeSchema(): SqlSchema {
  const tables = [STARTUPS];
  const modules = [
    {
      includeKey: 'android.startup.startups',
      functions: [
        {name: 'startup_dur', description: '', args: [], returnType: 'LONG'},
      ],
      tableFunctions: [],
      macros: [],
    },
  ];
  return {
    listTables: () => tables,
    listTablesNames: () => tables.map((t) => t.name),
    getTable: (n) =>
      tables.find((t) => t.name.toLowerCase() === n.toLowerCase()),
    listModules: () => modules,
  };
}

const diag = (over: Partial<EngineDiagnostic>): EngineDiagnostic => ({
  from: 0,
  to: 5,
  severity: 'warning',
  message: 'msg',
  detail: null,
  ...over,
});

beforeEach(() => {
  vi.mocked(applySchema).mockReset();
  vi.mocked(engineDiagnostics).mockReset();
  vi.mocked(engineHasSchema).mockReset();
});

describe('perfetto sql diagnostics', () => {
  test('returns [] when engine not ready', () => {
    vi.mocked(engineDiagnostics).mockReturnValue(undefined);
    const src = createPerfettoSqlDiagnosticsSource(fakeSchema);
    expect(src('select 1')).toEqual([]);
    expect(vi.mocked(applySchema)).toHaveBeenCalled();
  });

  test('before a schema is applied: suppress unknown_*, keep parse errors', () => {
    vi.mocked(engineHasSchema).mockReturnValue(false);
    vi.mocked(engineDiagnostics).mockReturnValue([
      diag({
        message: 'unknown table',
        detail: {kind: 'unknown_table', name: 'foo'},
      }),
      diag({message: 'syntax error', severity: 'error', detail: null}),
    ]);
    const out = createPerfettoSqlDiagnosticsSource(fakeSchema)('x');
    expect(out.map((d) => d.message)).toEqual(['syntax error']);
  });

  test('with schema: unknown table kept; stdlib table gets INCLUDE help', () => {
    vi.mocked(engineHasSchema).mockReturnValue(true);
    vi.mocked(engineDiagnostics).mockReturnValue([
      diag({
        message: 'unknown table',
        detail: {kind: 'unknown_table', name: 'android_startups'},
      }),
    ]);
    const out = createPerfettoSqlDiagnosticsSource(fakeSchema)(
      'select * from android_startups',
    );
    expect(out).toHaveLength(1);
    expect(out[0].help).toBe(
      'Add: INCLUDE PERFETTO MODULE android.startup.startups;',
    );
  });

  test('with schema: unknown_function suppressed for catalog-known callables', () => {
    vi.mocked(engineHasSchema).mockReturnValue(true);
    vi.mocked(engineDiagnostics).mockReturnValue([
      diag({detail: {kind: 'unknown_function', name: 'startup_dur'}}),
      diag({detail: {kind: 'unknown_function', name: 'totally_made_up'}}),
    ]);
    const out = createPerfettoSqlDiagnosticsSource(fakeSchema)('x');
    expect(out).toHaveLength(1); // startup_dur suppressed, made-up kept
  });
});
