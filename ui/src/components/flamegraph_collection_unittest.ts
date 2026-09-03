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
import {describe, expect, test} from 'vitest';

import {ensureExists} from '../base/assert';
import type {Row, SqlValue} from '../trace_processor/query_result';
import type {ColumnDef, ColumnSchema} from './widgets/datagrid/datagrid_schema';
import {
  buildCollectionGridSchema,
  buildEntryKeyResolver,
  shouldHandleStepKey,
} from './flamegraph_collection';

const COLUMNS = [
  {field: 'c0', title: 'profile', kind: 'id'},
  {field: 'c1', title: 'cpu (ns)', kind: 'numeric', unit: 'ns'},
] as const;
const GRID_COLUMNS = COLUMNS.map((c) => ({id: c.field, field: c.field}));
const CURRENT = '.pf-flamegraph-collection__entry--current';

// Defaults to the pprof case, where the key is the displayed identifier.
function schemaFor(
  resolveKey: (row: Row) => string | undefined = (row) => String(row['c0']),
  currentKey?: string,
  onJump: (key: string) => void = () => {},
) {
  return buildCollectionGridSchema(COLUMNS, resolveKey, currentKey, onJump);
}

// Renders one cell; `row` defaults to one carrying just that value.
function cell(
  schema: ColumnSchema,
  field: string,
  value: SqlValue,
  row?: Row,
): HTMLElement {
  const render = ensureExists((schema[field] as ColumnDef).cellRenderer);
  const el = document.createElement('div');
  m.render(el, render(value, row ?? ({[field]: value} as Row)) as m.Children);
  return el;
}

describe('buildCollectionGridSchema', () => {
  test('maps column kinds onto grid column types', () => {
    const schema = schemaFor();
    expect(schema['c0']).toMatchObject({columnType: 'identifier'});
    expect(schema['c1']).toMatchObject({columnType: 'quantitative'});
  });

  test('numeric cells format through displaySize with the column unit', () => {
    const schema = schemaFor();
    expect(cell(schema, 'c1', 1500000).textContent).toBe('1.50 ms');
    expect(cell(schema, 'c1', null).textContent).toBe('');
  });

  test('id cells highlight and navigate by resolved key, not by label', () => {
    // Consumers may show a label while keying entries by something else.
    const rows = [
      {key: 'utid=12', c0: 'traced_probes', c1: 5},
      {key: 'utid=13', c0: 'surfaceflinger', c1: 7},
    ];
    const jumped: string[] = [];
    const schema = schemaFor(
      buildEntryKeyResolver(rows, GRID_COLUMNS, (row) => String(row['key'])),
      'utid=12',
      (key) => jumped.push(key),
    );

    const current = cell(schema, 'c0', rows[0].c0, rows[0]);
    expect(current.textContent).toBe('traced_probes');
    expect(current.querySelector(CURRENT)).toBeTruthy();

    const other = cell(schema, 'c0', rows[1].c0, rows[1]);
    expect(other.querySelector(CURRENT)).toBeNull();
    other
      .querySelector('span')
      ?.dispatchEvent(new window.MouseEvent('click', {bubbles: true}));
    expect(jumped).toEqual(['utid=13']);
  });

  test('id cells render unresolvable rows inert', () => {
    // DataGrid renders its aggregate-totals row through the same renderer.
    const schema = schemaFor(
      () => undefined,
      'utid=12',
      () => {
        throw new Error('unresolvable rows must not navigate');
      },
    );
    const el = cell(schema, 'c0', 42, {} as Row);
    expect(el.textContent).toBe('42');
    expect(el.querySelector('.pf-flamegraph-collection__entry')).toBeNull();
  });
});

describe('buildEntryKeyResolver', () => {
  // Two entries share a label; two are identical across every column.
  const resolve = buildEntryKeyResolver(
    [
      {key: 'utid=12', c0: 'foo 1', c1: 5},
      {key: 'utid=13', c0: 'foo 1', c1: 7},
      {key: 'utid=98', c0: 'dup', c1: 9},
      {key: 'utid=99', c0: 'dup', c1: 9},
    ],
    GRID_COLUMNS,
    (row) => String(row['key']),
  );

  test.each<[string, Row, string | undefined]>([
    ['resolves a row to its key', {c0: 'foo 1', c1: 5}, 'utid=12'],
    ['separates rows sharing a label', {c0: 'foo 1', c1: 7}, 'utid=13'],
    ['drops unknown rows', {c0: 'bar', c1: 5}, undefined],
    ['drops indistinguishable rows', {c0: 'dup', c1: 9}, undefined],
    ['drops rows missing a column', {c0: 'foo 1'}, undefined],
    ['drops the empty totals row', {}, undefined],
  ])('%s', (_name, row, expected) => {
    expect(resolve(row)).toBe(expected);
  });

  test('maps renderer aliases onto data fields', () => {
    // Renderer rows are keyed by column id, data rows by column field.
    const byAlias = buildEntryKeyResolver(
      [{key: 'utid=12', raw: 'foo 1'}],
      [{id: 'alias0', field: 'raw'}],
      (row) => String(row['key']),
    );
    expect(byAlias({alias0: 'foo 1'})).toBe('utid=12');
  });
});

describe('shouldHandleStepKey', () => {
  const visible = {offsetParent: {}} as unknown as HTMLElement;
  const hidden = {offsetParent: null} as unknown as HTMLElement;
  const ev = (key: string, opts: {mod?: boolean; target?: unknown} = {}) =>
    ({
      key,
      ctrlKey: opts.mod ?? false,
      metaKey: false,
      altKey: false,
      target: opts.target,
    }) as unknown as KeyboardEvent;

  test.each<[string, KeyboardEvent, HTMLElement | undefined, boolean, number]>([
    ['steps on left arrow', ev('ArrowLeft'), visible, false, 2],
    ['steps on right arrow', ev('ArrowRight'), visible, false, 2],
    ['steps outside form controls', ev('ArrowLeft', {target: document.createElement('div')}), visible, false, 2], // prettier-ignore
  ])('%s', (_name, event, el, merge, count) => {
    expect(shouldHandleStepKey(event, el, merge, count)).toBe(true);
  });

  test.each<[string, KeyboardEvent, HTMLElement | undefined, boolean, number]>([
    ['ignores other keys', ev('a'), visible, false, 2],
    ['ignores modifiers', ev('ArrowLeft', {mod: true}), visible, false, 2],
    ['ignores merged mode', ev('ArrowLeft'), visible, true, 2],
    ['ignores a lone entry', ev('ArrowLeft'), visible, false, 1],
    ['ignores hidden collections', ev('ArrowLeft'), hidden, false, 2],
    ['ignores unmounted collections', ev('ArrowLeft'), undefined, false, 2],
    ['ignores typing in a form control', ev('ArrowLeft', {target: document.createElement('input')}), visible, false, 2], // prettier-ignore
  ])('%s', (_name, event, el, merge, count) => {
    expect(shouldHandleStepKey(event, el, merge, count)).toBe(false);
  });
});
