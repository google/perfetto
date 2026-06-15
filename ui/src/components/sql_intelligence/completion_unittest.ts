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
import type {CompletionContextLike} from '../../widgets/editor';
import {
  createPerfettoSqlCompletionSource,
  includedModules,
  scanReferencedTables,
  stripSqlNoise,
} from './completion';
import {flattenCallables, type SqlSchema, type SqlSchemaTable} from './schema';

const SLICE: SqlSchemaTable = {
  name: 'slice',
  description: 'Slices',
  columns: [{name: 'id'}, {name: 'ts'}, {name: 'dur'}, {name: 'name'}],
};
const STARTUPS: SqlSchemaTable = {
  name: 'android_startups',
  includeKey: 'android.startup.startups',
  description: 'Startups',
  columns: [{name: 'startup_id'}, {name: 'package'}],
};

function fakeSchema(): SqlSchema {
  const tables = [SLICE, STARTUPS];
  const modules = [
    {includeKey: 'prelude', functions: [], tableFunctions: [], macros: []},
    {
      includeKey: 'android.startup.startups',
      functions: [
        {
          name: 'startup_dur',
          description: 'Startup duration',
          args: [{name: 'id', type: 'LONG'}],
          returnType: 'LONG',
        },
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

// Minimal CompletionContextLike with CM-like matchBefore semantics.
function ctx(
  text: string,
  pos = text.length,
  explicit = true,
): CompletionContextLike {
  return {
    pos,
    explicit,
    state: {
      doc: {toString: () => text, length: text.length},
    } as CompletionContextLike['state'],
    matchBefore: (re: RegExp) => {
      const before = text.slice(0, pos);
      const m = before.match(
        new RegExp(re.source + '$', re.flags.replace('g', '')),
      );
      if (!m) return null;
      return {from: pos - m[0].length, to: pos, text: m[0]};
    },
  };
}

const labels = (r: {options: ReadonlyArray<{label: string}>} | null) =>
  (r?.options ?? []).map((o) => o.label);

describe('perfetto sql completion', () => {
  const src = createPerfettoSqlCompletionSource(fakeSchema);

  test('INCLUDE PERFETTO MODULE suggests module names', () => {
    const r = src(ctx('include perfetto module android'));
    expect(labels(r)).toContain('android.startup.startups');
    // prelude is filtered out.
    expect(labels(r)).not.toContain('prelude');
  });

  test('table. suggests that table columns', () => {
    const r = src(ctx('select slice.'));
    expect(labels(r)).toEqual(['id', 'ts', 'dur', 'name']);
  });

  test('alias. resolves through FROM alias', () => {
    const text = 'select s. from slice s';
    const r = src(ctx(text, 'select s.'.length));
    expect(labels(r)).toEqual(['id', 'ts', 'dur', 'name']);
  });

  test('after FROM, tables are offered and ranked first', () => {
    const r = src(ctx('select * from sl'));
    const slice = r?.options.find((o) => o.label === 'slice');
    expect(slice).toBeDefined();
    expect(slice!.boost ?? 0).toBeGreaterThan(0);
  });

  test('general position offers functions + referenced columns', () => {
    // Cursor after FROM, so the referenced table (slice) is in textBefore.
    const r = src(ctx('select * from slice where '));
    const ls = labels(r);
    expect(ls).toContain('startup_dur'); // stdlib function
    expect(ls).toContain('id'); // column of referenced table slice
    expect(ls).toContain('SELECT'); // keyword
  });
});

describe('sql text helpers', () => {
  test('stripSqlNoise blanks strings + comments', () => {
    const out = stripSqlNoise(
      "select 'from x' /* from y */ from slice -- from z",
    );
    expect(out).not.toMatch(/from x/);
    expect(out).not.toMatch(/from y/);
    expect(out).not.toMatch(/from z/);
    expect(out).toMatch(/from slice/);
  });

  test('scanReferencedTables ignores CTEs + strings', () => {
    const {tables} = scanReferencedTables(
      "with cte as (select 1) select * from slice join cte using(id) where x = 'from foo'",
    );
    expect(tables.has('slice')).toBe(true);
    expect(tables.has('cte')).toBe(false);
    expect(tables.has('foo')).toBe(false);
  });

  test('includedModules parses include statements', () => {
    const inc = includedModules(
      'INCLUDE PERFETTO MODULE android.startup.startups;',
    );
    expect(inc.has('android.startup.startups')).toBe(true);
  });
});

describe('flattenCallables', () => {
  test('flattens module callables and memoizes per schema', () => {
    const schema = fakeSchema();
    const a = flattenCallables(schema);
    const b = flattenCallables(schema);
    expect(a).toBe(b); // memoized
    expect(a.functions.map((f) => f.name)).toEqual(['startup_dur']);
  });
});
