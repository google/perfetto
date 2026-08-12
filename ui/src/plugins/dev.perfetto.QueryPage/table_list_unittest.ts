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
import {TableList} from './table_list';
import type {
  SqlModules,
  SqlTable,
} from '../dev.perfetto.SqlModules/sql_modules';

function makeSqlModules(names: string[]): SqlModules {
  const tables = names.map(
    (name) =>
      ({
        name,
        description: '',
        type: 'table',
        columns: [],
        getTableColumns: () => [],
      }) as unknown as SqlTable,
  );
  return {
    listTables: () => tables,
    listModules: () => [],
    listTablesNames: () => names,
    getTable: () => undefined,
    getModuleForTable: () => undefined,
    isModuleDisabled: () => false,
    getDisabledModules: () => new Set<string>(),
    ensureInitialized: async () => {},
  } as unknown as SqlModules;
}

// Types `text` into the search box one character at a time, then deletes it,
// re-rendering after every keystroke. Each render re-runs the fuzzy filter and
// re-diffs the keyed accordion, which is what triggered the original crash.
function typeSearch(root: HTMLElement, comp: m.Component, text: string) {
  const feed = (value: string) => {
    const input = root.querySelector('input');
    if (input) {
      input.value = value;
      input.dispatchEvent(new Event('input', {bubbles: true}));
    }
    m.render(root, m(comp));
  };
  for (let i = 1; i <= text.length; i++) feed(text.slice(0, i));
  for (let i = text.length - 1; i >= 0; i--) feed(text.slice(0, i));
}

// A realistic set of stdlib-like names with word structure the fuzzy finder
// can rank and reorder.
function makeNames(): string[] {
  const prefixes = [
    'cpu',
    'thread',
    'process',
    'slice',
    'memory',
    'android',
    'linux',
    'counter',
    'sched',
    'battery',
  ];
  const suffixes = [
    'counters',
    'table',
    'state',
    'info',
    'summary',
    'usage',
    'events',
    'stats',
    'metadata',
    'residency',
  ];
  const names: string[] = [];
  for (const p of prefixes) for (const s of suffixes) names.push(`${p}_${s}`);
  return names;
}

describe('TableList', () => {
  afterEach(() => {
    document.body.innerHTML = '';
  });

  test('renders every table and does not crash with duplicate names', () => {
    // A registered SQL package can declare a table name that already exists in
    // the stdlib, so listTables() may return the same name more than once.
    // Duplicate mithril keys used to crash the accordion's keyed diff with
    // "Cannot read properties of null (reading 'tag')" as the fuzzy results
    // reordered on each keystroke. Both entries must still render.
    const names = makeNames();
    names.splice(15, 0, names[3]);
    names.splice(40, 0, names[3]);
    names.splice(70, 0, names[8]);

    const sqlModules = makeSqlModules(names);
    const comp = {view: () => m(TableList, {sqlModules})};
    const root = document.createElement('div');
    document.body.appendChild(root);
    m.render(root, m(comp));

    // Every table is shown, including the duplicates (nothing is collapsed).
    expect(root.querySelectorAll('.pf-accordion__item')).toHaveLength(
      names.length,
    );

    expect(() => {
      for (const t of ['slice', 'cpu', 'thread', 'counter', 's', 'state']) {
        typeSearch(root, comp, t);
      }
    }).not.toThrow();
  });
});
