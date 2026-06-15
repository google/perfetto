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

import {describe, expect, test} from 'vitest';
import type {Engine} from '../../trace_processor/engine';
import type {
  SqlSchema,
  SqlSchemaTable,
} from '../../components/sql_intelligence/schema';
import {LiveSchemaCache, withExtraTables} from './live_schema';

const SLICE: SqlSchemaTable = {
  name: 'slice',
  description: 'Slices',
  columns: [{name: 'id'}],
};

function baseSchema(): SqlSchema {
  const tables = [SLICE];
  const modules = [
    {includeKey: 'prelude', functions: [], tableFunctions: [], macros: []},
  ];
  return {
    listTables: () => tables,
    listTablesNames: () => tables.map((t) => t.name),
    getTable: (n) =>
      tables.find((t) => t.name.toLowerCase() === n.toLowerCase()),
    listModules: () => modules,
  };
}

// A fake engine whose pragma_table_info query yields `cols`.
function fakeEngine(cols: string[]): Engine {
  return {
    query: async (_sql: string) => {
      let i = 0;
      return {
        iter: (_spec: unknown) => ({
          valid: () => i < cols.length,
          next: () => {
            i++;
          },
          get name() {
            return cols[i];
          },
        }),
      };
    },
  } as unknown as Engine;
}

describe('withExtraTables', () => {
  test('returns the base schema unchanged when there are no extras', () => {
    const base = baseSchema();
    expect(withExtraTables(base, [])).toBe(base);
  });

  test('layers extra tables over the base (case-insensitive, base wins)', () => {
    const foo: SqlSchemaTable = {
      name: 'foo',
      description: '',
      columns: [{name: 'x'}],
    };
    const merged = withExtraTables(baseSchema(), [foo]);
    expect(merged.listTablesNames()).toContain('foo');
    expect(merged.getTable('FOO')?.name).toBe('foo');
    expect(merged.getTable('slice')?.name).toBe('slice'); // base still resolvable
    expect(merged.listModules().map((m) => m.includeKey)).toEqual(['prelude']);
  });
});

describe('LiveSchemaCache.recordFromExecutedSql', () => {
  test('records a CREATE PERFETTO TABLE with its columns', async () => {
    const cache = new LiveSchemaCache();
    await cache.recordFromExecutedSql(
      'CREATE PERFETTO TABLE foo AS SELECT 1 AS a, 2 AS b',
      fakeEngine(['a', 'b']),
    );
    const t = cache.getTables().find((x) => x.name === 'foo');
    expect(t).toBeDefined();
    expect(t!.columns.map((c) => c.name)).toEqual(['a', 'b']);
  });

  test('records CREATE OR REPLACE PERFETTO VIEW', async () => {
    const cache = new LiveSchemaCache();
    await cache.recordFromExecutedSql(
      'create or replace perfetto view bar as select 1 as x',
      fakeEngine(['x']),
    );
    expect(cache.getTables().some((x) => x.name === 'bar')).toBe(true);
  });

  test('getTables returns a stable reference until the tables change', async () => {
    const cache = new LiveSchemaCache();
    const a = cache.getTables();
    expect(cache.getTables()).toBe(a); // stable across calls
    await cache.recordFromExecutedSql(
      'create perfetto table foo as select 1 as a',
      fakeEngine(['a']),
    );
    const b = cache.getTables();
    expect(b).not.toBe(a); // new snapshot after a mutation
    expect(cache.getTables()).toBe(b); // stable again
  });

  test('ignores non-CREATE queries', async () => {
    const cache = new LiveSchemaCache();
    await cache.recordFromExecutedSql('select * from slice', fakeEngine(['a']));
    expect(cache.getTables()).toEqual([]);
  });

  test('ignores CREATE inside comments/strings', async () => {
    const cache = new LiveSchemaCache();
    await cache.recordFromExecutedSql(
      "-- create perfetto table ghost\nselect 'create perfetto table ghost2'",
      fakeEngine(['a']),
    );
    expect(cache.getTables()).toEqual([]);
  });
});
