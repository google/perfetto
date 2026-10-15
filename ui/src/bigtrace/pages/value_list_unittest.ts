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

import {afterEach, beforeEach, describe, expect, test, vi} from 'vitest';
import m from 'mithril';
import type {Filter} from '../../components/widgets/datagrid/model';
import {
  BigtraceHttpError,
  QueryCancelledError,
} from '../query/bigtrace_query_client';
import {
  type ColumnValuesLoader,
  VALUE_LIST_LIMIT,
} from '../query/column_values_loader';
import {
  SEARCH_DEBOUNCE_MS,
  ValueList,
  type ValueListAttrs,
  highlight,
  orderValues,
} from './value_list';

interface Call {
  column: string;
  facet: ReadonlyArray<Filter>;
  search: string;
  signal?: AbortSignal;
  resolve: (v: ReadonlyArray<string>) => void;
  reject: (e: unknown) => void;
}

// A loader whose requests are settled by the test. By default it rejects
// with QueryCancelledError when its signal aborts, as the real client does.
function makeLoader(opts: {honourAbort?: boolean} = {}) {
  const calls: Call[] = [];
  const load: ColumnValuesLoader = (column, facet, search, signal) =>
    new Promise((resolve, reject) => {
      calls.push({column, facet, search, signal, resolve, reject});
      if (opts.honourAbort !== false) {
        signal?.addEventListener('abort', () =>
          reject(new QueryCancelledError()),
        );
      }
    });
  return {calls, load};
}

interface Harness {
  root: HTMLElement;
  attrs: {
    facet: ReadonlyArray<Filter>;
    search: string | undefined;
    selected: Set<string>;
    pinSelected?: boolean;
  };
  redraw: () => void;
  unmount: () => void;
  rows: () => string[];
  notes: () => string[];
  text: () => string;
}

function mount(load: ColumnValuesLoader): Harness {
  const root = document.createElement('div');
  document.body.appendChild(root);
  const attrs = {
    facet: [] as ReadonlyArray<Filter>,
    search: undefined as string | undefined,
    selected: new Set<string>(),
    pinSelected: undefined as boolean | undefined,
  };
  m.mount(root, {
    view: () =>
      m(ValueList, {
        column: 'col',
        load,
        facet: attrs.facet,
        search: attrs.search,
        selected: attrs.selected,
        pinSelected: attrs.pinSelected,
        onToggle: (v, on) =>
          on ? attrs.selected.add(v) : attrs.selected.delete(v),
      } satisfies ValueListAttrs),
  });
  const texts = (sel: string) =>
    [...root.querySelectorAll(sel)].map((e) => (e.textContent ?? '').trim());
  return {
    root,
    attrs,
    redraw: () => m.redraw.sync(),
    unmount: () => {
      m.mount(root, null);
      root.remove();
    },
    rows: () => texts('.pf-bt-value-list__item'),
    notes: () => texts('.pf-bt-value-list__note'),
    text: () => root.textContent ?? '',
  };
}

const flush = () => vi.advanceTimersByTimeAsync(0);

describe('highlight', () => {
  test('wraps the first case-insensitive match', () => {
    expect(highlight('Pixel', '')).toBe('Pixel');
    expect(highlight('Pixel', 'zz')).toBe('Pixel');
    const root = document.createElement('div');
    m.render(root, highlight('Pixel 9 Pro', 'pro'));
    expect(root.innerHTML).toBe('Pixel 9 <mark>Pro</mark>');
  });
});

describe('orderValues', () => {
  const fetched = ['c', 'a', 'd'];
  const selected = new Set(['b', 'a']);

  test('pinned: picked first and sorted, then the matching rest', () => {
    expect(orderValues(fetched, selected, '', true)).toEqual([
      'a',
      'b',
      'c',
      'd',
    ]);
    // Picked values ignore the search; the rest is narrowed by it.
    expect(orderValues(fetched, selected, 'd', true)).toEqual(['a', 'b', 'd']);
  });

  test('unpinned: backend order kept, missing picks appended', () => {
    expect(orderValues(fetched, selected, '', false)).toEqual([
      'c',
      'a',
      'd',
      'b',
    ]);
    // A picked value the search would hide stays in place.
    expect(orderValues(fetched, selected, 'd', false)).toEqual(['a', 'd', 'b']);
    expect(orderValues([], new Set(), '', false)).toEqual([]);
  });
});

describe('ValueList', () => {
  let h: Harness | undefined;
  beforeEach(() => vi.useFakeTimers());
  afterEach(() => {
    h?.unmount();
    h = undefined;
    vi.useRealTimers();
  });

  test('fetches once on mount with an empty search', () => {
    const {calls, load} = makeLoader();
    h = mount(load);
    expect(calls).toHaveLength(1);
    expect(calls[0].search).toBe('');
    expect(vi.getTimerCount()).toBe(0);
    expect(h.notes()).toEqual(['Loading…']);
  });

  test('a typing burst becomes one request after the debounce', async () => {
    const {calls, load} = makeLoader();
    h = mount(load);
    calls[0].resolve(['Pixel 8', 'Pixel 9', 'Galaxy']);
    await flush();
    h.redraw();
    expect(h.rows()).toEqual(['Pixel 8', 'Pixel 9', 'Galaxy']);
    for (const s of ['a', 'ab', 'abc']) {
      h.attrs.search = s;
      h.redraw();
      await vi.advanceTimersByTimeAsync(100);
    }
    // An unrelated redraw with the same pending text must not reset it.
    h.redraw();
    await vi.advanceTimersByTimeAsync(SEARCH_DEBOUNCE_MS - 101);
    h.redraw();
    expect(calls).toHaveLength(1);
    await vi.advanceTimersByTimeAsync(1);
    h.redraw();
    expect(calls).toHaveLength(2);
    expect(calls[1].search).toBe('abc');
  });

  test('reverting to the sent search cancels the pending timer', async () => {
    const {calls, load} = makeLoader();
    h = mount(load);
    calls[0].resolve(['x']);
    await flush();
    h.redraw();
    h.attrs.search = 'a';
    h.redraw();
    expect(vi.getTimerCount()).toBe(1);
    h.attrs.search = '';
    h.redraw();
    expect(vi.getTimerCount()).toBe(0);
    await vi.advanceTimersByTimeAsync(1000);
    h.redraw();
    expect(calls).toHaveLength(1);
  });

  test('a facet change aborts the request in flight and drops its answer', async () => {
    const {calls, load} = makeLoader({honourAbort: false});
    h = mount(load);
    const first = calls[0];
    h.attrs.facet = [{field: 'ff', op: 'in', value: ['phone']}];
    h.redraw();
    expect(first.signal?.aborted).toBe(true);
    expect(calls).toHaveLength(2);
    expect(calls[1].facet).toEqual(h.attrs.facet);
    // The loader ignored the abort and answers late: not shown.
    first.resolve(['STALE']);
    await flush();
    h.redraw();
    expect(h.rows()).toEqual([]);
    expect(h.notes()).toEqual(['Loading…']);
    calls[1].resolve(['fresh']);
    await flush();
    h.redraw();
    expect(h.rows()).toEqual(['fresh']);
    expect(h.notes()).toEqual([]);
  });

  test('the cancellation of a superseded request is not an error', async () => {
    const {calls, load} = makeLoader();
    h = mount(load);
    h.attrs.facet = [{field: 'ff', op: 'in', value: ['phone']}];
    h.redraw();
    await flush();
    h.redraw();
    expect(calls).toHaveLength(2);
    expect(h.text()).not.toContain('Couldn');
    expect(h.notes()).toEqual(['Loading…']);
  });

  test('searching keeps the last list, narrowed locally, while updating', async () => {
    const {calls, load} = makeLoader();
    h = mount(load);
    calls[0].resolve(['Pixel 8', 'Pixel 9', 'Galaxy']);
    await flush();
    h.redraw();
    h.attrs.search = 'pix';
    h.redraw();
    // Before the debounce: narrowed locally, nothing sent yet.
    expect(h.rows()).toEqual(['Pixel 8', 'Pixel 9']);
    expect(h.notes()).toEqual([]);
    await vi.advanceTimersByTimeAsync(SEARCH_DEBOUNCE_MS);
    h.redraw();
    expect(calls).toHaveLength(2);
    expect(h.rows()).toEqual(['Pixel 8', 'Pixel 9']);
    expect(h.notes()).toEqual(['Updating…']);
    expect(h.root.querySelectorAll('mark')).toHaveLength(2);
    calls[1].resolve(['Pixel 8']);
    await flush();
    h.redraw();
    expect(h.rows()).toEqual(['Pixel 8']);
  });

  test('picked values are pinned first, sorted, and survive a search', async () => {
    const {calls, load} = makeLoader();
    h = mount(load);
    h.attrs.pinSelected = true;
    h.attrs.selected.add('b');
    h.attrs.selected.add('a');
    calls[0].resolve(['c', 'a', 'd']);
    await flush();
    h.redraw();
    expect(h.rows()).toEqual(['a', 'b', 'c', 'd']);
    h.attrs.search = 'zzz';
    h.redraw();
    expect(h.rows()).toEqual(['a', 'b']);
  });

  test('without pinning, rows keep their order and ticking does not move them', async () => {
    const {calls, load} = makeLoader();
    h = mount(load);
    calls[0].resolve(['ALPHA', 'BETA', 'STABLE']);
    await flush();
    h.redraw();
    const boxes = h.root.querySelectorAll<HTMLInputElement>(
      'input[type=checkbox]',
    );
    boxes[1].click();
    h.redraw();
    expect(h.attrs.selected.has('BETA')).toBe(true);
    expect(h.rows()).toEqual(['ALPHA', 'BETA', 'STABLE']);
  });

  test('a failure is shown once, not refetched, and Retry asks again', async () => {
    const {calls, load} = makeLoader();
    h = mount(load);
    calls[0].reject(
      new BigtraceHttpError(400, '{"error":{"message":"bad col"}}'),
    );
    await flush();
    h.redraw();
    h.redraw();
    expect(calls).toHaveLength(1);
    expect(h.text()).toContain('Couldn’t load values.');
    expect(h.text()).toContain('bad col (HTTP 400)');
    expect(h.text()).not.toContain('No values match');
    const note = h.root.querySelector('.pf-bt-value-list__note--error');
    expect(note?.getAttribute('role')).toBe('alert');
    expect(note?.getAttribute('title')).toContain('HTTP 400');
    const retry = [...h.root.querySelectorAll('button')].find((b) =>
      (b.textContent ?? '').includes('Retry'),
    );
    expect(retry).toBeDefined();
    retry?.click();
    h.redraw();
    expect(calls).toHaveLength(2);
    expect(h.notes()).toEqual(['Loading…']);
    calls[1].resolve(['ok']);
    await flush();
    h.redraw();
    expect(h.rows()).toEqual(['ok']);
    expect(h.text()).not.toContain('Couldn');
  });

  test('a failure is forgotten when the facet changes', async () => {
    const {calls, load} = makeLoader();
    h = mount(load);
    calls[0].reject(new BigtraceHttpError(400, 'x'));
    await flush();
    h.redraw();
    h.attrs.facet = [{field: 'f', op: '=', value: 1}];
    h.redraw();
    expect(calls).toHaveLength(2);
    expect(h.text()).not.toContain('Couldn');
  });

  test('empty answers and the limit note', async () => {
    const {calls, load} = makeLoader();
    h = mount(load);
    calls[0].resolve([]);
    await flush();
    h.redraw();
    expect(h.notes()).toEqual(['No values match.']);
    h.unmount();

    const under = makeLoader();
    h = mount(under.load);
    under.calls[0].resolve(
      Array.from({length: VALUE_LIST_LIMIT - 1}, (_, i) => `v${i}`),
    );
    await flush();
    h.redraw();
    expect(h.text()).not.toContain('Showing the first');
    h.unmount();

    const at = makeLoader();
    h = mount(at.load);
    at.calls[0].resolve(
      Array.from({length: VALUE_LIST_LIMIT}, (_, i) => `v${i}`),
    );
    await flush();
    h.redraw();
    expect(h.text()).toContain(
      `Showing the first ${VALUE_LIST_LIMIT} — type to narrow down.`,
    );
  });

  test('unmounting aborts the request, clears the timer and stays quiet', async () => {
    const {calls, load} = makeLoader();
    h = mount(load);
    h.attrs.search = 'q';
    h.redraw();
    expect(vi.getTimerCount()).toBe(1);
    const signal = calls[0].signal;
    const redraw = vi.spyOn(m, 'redraw');
    h.unmount();
    h = undefined;
    expect(signal?.aborted).toBe(true);
    expect(vi.getTimerCount()).toBe(0);
    await flush();
    expect(redraw).not.toHaveBeenCalled();
    redraw.mockRestore();
  });

  test('toggling a row does not refetch', async () => {
    const {calls, load} = makeLoader();
    h = mount(load);
    calls[0].resolve(['a', 'b']);
    await flush();
    h.redraw();
    const box = h.root.querySelector<HTMLInputElement>('input[type=checkbox]');
    box?.click();
    h.redraw();
    expect(h.attrs.selected.has('a')).toBe(true);
    expect(calls).toHaveLength(1);
  });
});
