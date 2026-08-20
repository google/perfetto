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

import type {Row, SqlValue} from '../trace_processor/query_result';
import type {ColumnDef, ColumnSchema} from './widgets/datagrid/datagrid_schema';
import {
  buildCollectionGridSchema,
  buildEntryKeyResolver,
  FLAMEGRAPH_COLLECTION_STATE_SCHEMA,
  shouldHandleStepKey,
} from './flamegraph_collection';

// Renders one cell of the schema into a detached element and returns it.
function renderCell(
  schema: ColumnSchema,
  field: string,
  value: SqlValue,
  row?: Row,
): HTMLElement {
  const def = schema[field] as ColumnDef;
  const renderer = def.cellRenderer;
  if (renderer === undefined) {
    throw new Error(`no cellRenderer for ${field}`);
  }
  const el = document.createElement('div');
  m.render(el, renderer(value, row ?? ({[field]: value} as Row)) as m.Children);
  return el;
}

describe('FLAMEGRAPH_COLLECTION_STATE_SCHEMA', () => {
  test('fills defaults for an empty object', () => {
    const state = FLAMEGRAPH_COLLECTION_STATE_SCHEMA.parse({});
    expect(state.merge).toBe(true);
    expect(state.filters).toEqual([]);
    expect(state.columns).toBeUndefined();
    expect(state.flamegraphState).toBeUndefined();
  });

  test('accepts persisted grid columns and filters', () => {
    const state = FLAMEGRAPH_COLLECTION_STATE_SCHEMA.parse({
      merge: false,
      columns: [
        {id: 'c0', field: 'c0'},
        {id: 'c1', field: 'c1', sort: 'DESC'},
      ],
      filters: [
        {field: 'c1', op: '>', value: 10},
        {field: 'c0', op: 'glob', value: '*foo*'},
        {field: 'c0', op: 'in', value: ['a', 'b']},
        {field: 'c2', op: 'is null'},
      ],
    });
    expect(state.merge).toBe(false);
    expect(state.columns).toHaveLength(2);
    expect(state.filters).toHaveLength(4);
  });

  test('rejects malformed filters', () => {
    expect(
      FLAMEGRAPH_COLLECTION_STATE_SCHEMA.safeParse({
        filters: [{field: 'c1', op: 'between', value: 10}],
      }).success,
    ).toBe(false);
    expect(
      FLAMEGRAPH_COLLECTION_STATE_SCHEMA.safeParse({
        filters: [{field: 'c1', op: '=', value: {}}],
      }).success,
    ).toBe(false);
  });
});

describe('buildCollectionGridSchema', () => {
  const columns = [
    {field: 'c0', title: 'profile', kind: 'id'},
    {field: 'c1', title: 'cpu (ns)', kind: 'numeric', unit: 'ns'},
    {field: 'c2', title: 'device', kind: 'categorical'},
  ] as const;

  test('maps column kinds onto grid column types', () => {
    const schema = buildCollectionGridSchema(
      columns,
      (row) => String(row['c0']),
      undefined,
      () => {},
    );
    expect(schema['c0']).toMatchObject({columnType: 'identifier'});
    expect(schema['c1']).toMatchObject({columnType: 'quantitative'});
    expect(schema['c2']).toMatchObject({columnType: 'text'});
  });

  test('numeric cells format through displaySize with the column unit', () => {
    const schema = buildCollectionGridSchema(
      columns,
      (row) => String(row['c0']),
      undefined,
      () => {},
    );
    expect(renderCell(schema, 'c1', 1500000).textContent).toBe('1.50 ms');
    expect(renderCell(schema, 'c1', null).textContent).toBe('');
  });

  test('id cells highlight the current entry and navigate by resolved key', () => {
    // StackSamples/HeapProfile display a human label (thread/process name)
    // while the entry key is a context key like "utid=12". Renderer rows
    // carry only the visible columns, so the key is recovered through
    // buildEntryKeyResolver from the full rows.
    const jumped: string[] = [];
    const fullRows = [
      {key: 'utid=12', c0: 'traced_probes [session 1]', c1: 5, c2: 'a'},
      {key: 'utid=13', c0: 'surfaceflinger', c1: 7, c2: 'b'},
    ];
    const gridColumns = columns.map((c) => ({id: c.field, field: c.field}));
    const schema = buildCollectionGridSchema(
      columns,
      buildEntryKeyResolver(fullRows, gridColumns, (row) => String(row['key'])),
      'utid=12',
      (k) => jumped.push(k),
    );
    const current = renderCell(schema, 'c0', 'traced_probes [session 1]', {
      c0: 'traced_probes [session 1]',
      c1: 5,
      c2: 'a',
    });
    expect(current.textContent).toBe('traced_probes [session 1]');
    expect(
      current.querySelector('.pf-flamegraph-collection__entry--current'),
    ).toBeTruthy();
    const other = renderCell(schema, 'c0', 'surfaceflinger', {
      c0: 'surfaceflinger',
      c1: 7,
      c2: 'b',
    });
    expect(
      other.querySelector('.pf-flamegraph-collection__entry--current'),
    ).toBeNull();
    other
      .querySelector('span')
      ?.dispatchEvent(new window.MouseEvent('click', {bubbles: true}));
    expect(jumped).toEqual(['utid=13']);
  });

  test('id cells render unresolvable rows inert', () => {
    // DataGrid invokes cellRenderer(totalValue, {}) for the totals row; the
    // resolver yields undefined for it, so no entry span is rendered.
    const schema = buildCollectionGridSchema(
      columns,
      () => undefined,
      'utid=12',
      () => {
        throw new Error('unresolvable rows must not navigate');
      },
    );
    const el = renderCell(schema, 'c0', 42, {} as Row);
    expect(el.textContent).toBe('42');
    expect(el.querySelector('.pf-flamegraph-collection__entry')).toBeNull();
  });
});

describe('buildEntryKeyResolver', () => {
  const gridColumns = [
    {id: 'c0', field: 'c0'},
    {id: 'c1', field: 'c1'},
  ];
  const entryKey = (row: Row) => String(row['key']);

  test('resolves renderer rows to their entry keys', () => {
    const resolve = buildEntryKeyResolver(
      [
        {key: 'utid=12', c0: 'foo 1', c1: 5},
        {key: 'utid=13', c0: 'foo 1', c1: 7},
      ],
      gridColumns,
      entryKey,
    );
    // Same label, different sample counts: still distinguishable.
    expect(resolve({c0: 'foo 1', c1: 5})).toBe('utid=12');
    expect(resolve({c0: 'foo 1', c1: 7})).toBe('utid=13');
    expect(resolve({c0: 'bar', c1: 5})).toBeUndefined();
  });

  test('rows indistinguishable across visible columns resolve to nothing', () => {
    // A recycled tid can produce two contexts with identical labels and
    // totals; guessing would jump to the wrong entry, so neither resolves.
    const resolve = buildEntryKeyResolver(
      [
        {key: 'utid=12', c0: 'foo 1', c1: 5},
        {key: 'utid=99', c0: 'foo 1', c1: 5},
      ],
      gridColumns,
      entryKey,
    );
    expect(resolve({c0: 'foo 1', c1: 5})).toBeUndefined();
  });

  test('rows missing a visible column resolve to nothing', () => {
    const resolve = buildEntryKeyResolver(
      [{key: 'utid=12', c0: 'foo 1', c1: 5}],
      gridColumns,
      entryKey,
    );
    expect(resolve({} as Row)).toBeUndefined();
    expect(resolve({c0: 'foo 1'})).toBeUndefined();
  });

  test('maps renderer aliases onto data fields', () => {
    // Renderer rows are keyed by column id, data rows by column field.
    const resolve = buildEntryKeyResolver(
      [{key: 'utid=12', raw: 'foo 1'}],
      [{id: 'alias0', field: 'raw'}],
      entryKey,
    );
    expect(resolve({alias0: 'foo 1'})).toBe('utid=12');
  });
});

describe('shouldHandleStepKey', () => {
  const visibleEl = {offsetParent: {}} as unknown as HTMLElement;
  const hiddenEl = {offsetParent: null} as unknown as HTMLElement;

  function key(
    key: string,
    opts: {mod?: boolean; target?: unknown} = {},
  ): KeyboardEvent {
    return {
      key,
      ctrlKey: opts.mod ?? false,
      metaKey: false,
      altKey: false,
      target: opts.target,
    } as unknown as KeyboardEvent;
  }

  test('steps on arrow keys when visible, un-merged and multi-entry', () => {
    expect(shouldHandleStepKey(key('ArrowLeft'), visibleEl, false, 2)).toBe(
      true,
    );
    expect(shouldHandleStepKey(key('ArrowRight'), visibleEl, false, 2)).toBe(
      true,
    );
  });

  test('ignores other keys and modifiers', () => {
    expect(shouldHandleStepKey(key('a'), visibleEl, false, 2)).toBe(false);
    expect(
      shouldHandleStepKey(key('ArrowLeft', {mod: true}), visibleEl, false, 2),
    ).toBe(false);
  });

  test('ignores merged mode, single entries and hidden collections', () => {
    expect(shouldHandleStepKey(key('ArrowLeft'), visibleEl, true, 2)).toBe(
      false,
    );
    expect(shouldHandleStepKey(key('ArrowLeft'), visibleEl, false, 1)).toBe(
      false,
    );
    expect(shouldHandleStepKey(key('ArrowLeft'), hiddenEl, false, 2)).toBe(
      false,
    );
    expect(shouldHandleStepKey(key('ArrowLeft'), undefined, false, 2)).toBe(
      false,
    );
  });

  test('ignores keystrokes typed into form controls', () => {
    const input = document.createElement('input');
    expect(
      shouldHandleStepKey(
        key('ArrowLeft', {target: input}),
        visibleEl,
        false,
        2,
      ),
    ).toBe(false);
    const div = document.createElement('div');
    expect(
      shouldHandleStepKey(key('ArrowLeft', {target: div}), visibleEl, false, 2),
    ).toBe(true);
  });
});
