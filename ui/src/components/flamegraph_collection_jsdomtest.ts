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
import {afterEach, beforeEach, describe, expect, test, vi} from 'vitest';
import {prettyDOM} from '@testing-library/dom';

import type {Trace} from '../public/trace';
import type {Row} from '../trace_processor/query_result';
import type {QueryFlamegraphMetric} from './query_flamegraph';
import {
  FlamegraphCollection,
  FLAMEGRAPH_COLLECTION_STATE_SCHEMA,
} from './flamegraph_collection';
import type {
  FlamegraphCollectionAttrs,
  FlamegraphCollectionState,
} from './flamegraph_collection';

// The real FlamegraphPanel drives engine queries; stub it with a component
// that records the attrs of every render so tests can assert on the metrics
// and state handed to it. vi.hoisted because vi.mock factories are hoisted
// above imports.
const panel = vi.hoisted(() => {
  const state = {
    renders: [] as Array<{metrics?: unknown; state?: unknown}>,
    reset() {
      this.renders.length = 0;
    },
    last() {
      return this.renders[this.renders.length - 1];
    },
  };
  return state;
});

vi.mock('./flamegraph_panel', () => ({
  FlamegraphPanel: {
    view: ({attrs}: m.CVnode<{metrics?: unknown; state?: unknown}>) => {
      panel.renders.push({metrics: attrs.metrics, state: attrs.state});
      return m('.pf-test-fake-flamegraph-panel');
    },
  },
}));

// These tests render a FlamegraphCollection into jsdom and assert on the DOM
// plus the attrs captured by the stubbed FlamegraphPanel. The DataGrid
// virtualizes body rows off the viewport height (zero under jsdom), so only
// the grid chrome and the flame header/body are asserted; grid-cell
// interactions are covered by flamegraph_collection_unittest.ts.

const ROWS: Row[] = [
  {c0: 'a.pprof', c1: 100},
  {c0: 'b.pprof', c1: 25},
];

const COLUMNS = [
  {field: 'c0', title: 'profile', kind: 'id'},
  {field: 'c1', title: 'cpu (ns)', kind: 'numeric', unit: 'ns'},
] as const;

function metricFor(keys: ReadonlyArray<string>): QueryFlamegraphMetric {
  return {
    name: 'cpu (ns)',
    unit: 'ns',
    statement: `select ... where key in (${keys.join(',')})`,
  };
}

let container: HTMLElement;

beforeEach(() => {
  container = document.createElement('div');
  document.body.appendChild(container);
  panel.reset();
});

afterEach(() => {
  m.render(container, null);
  container.remove();
});

function dumpDom(): string {
  const out = prettyDOM();
  return typeof out === 'string' ? out : '';
}

interface Harness {
  rerender: () => void;
  state: () => FlamegraphCollectionState;
  setState: (s: FlamegraphCollectionState) => void;
  metricsForKeys: ReturnType<typeof vi.fn>;
  attrs: () => Partial<FlamegraphCollectionAttrs>;
}

// Renders a collection whose onStateChange stores the new state for the next
// explicit rerender — like a real store, where the redraw is scheduled rather
// than re-entrant. Fresh vnodes per rerender (m.render no-ops on an identical
// vnode object).
function renderCollection(
  initial?: Partial<FlamegraphCollectionState>,
  extraAttrs?: Partial<FlamegraphCollectionAttrs>,
): Harness {
  let state = FLAMEGRAPH_COLLECTION_STATE_SCHEMA.parse({...initial});
  const metricsForKeys = vi.fn((keys: ReadonlyArray<string>) =>
    keys.length > 0 ? [metricFor(keys)] : [],
  );
  const attrs = (): FlamegraphCollectionAttrs => ({
    trace: {} as Trace,
    rows: ROWS,
    columns: COLUMNS,
    entryKey: (row: Row) => String(row['c0']),
    metricsForKeys,
    entityName: 'profiles',
    state,
    onStateChange: (s: FlamegraphCollectionState) => {
      state = s;
    },
    ...extraAttrs,
  });
  const rerender = () => m.render(container, m(FlamegraphCollection, attrs()));
  rerender();
  // Flush the reconciled flamegraph state written back on first rebuild.
  rerender();
  return {
    rerender,
    state: () => state,
    setState: (s) => {
      state = s;
      rerender();
    },
    metricsForKeys,
    attrs,
  };
}

function summaryText(): string {
  return (
    container.querySelector('.pf-flamegraph-collection__summary')
      ?.textContent ?? ''
  );
}

function positionText(): string {
  return (
    container.querySelector('.pf-flamegraph-collection__flame-pos')
      ?.textContent ?? ''
  );
}

function toggleMerge(h: Harness): void {
  const input = container.querySelector<HTMLInputElement>(
    '.pf-flamegraph-collection__flame-head input[type=checkbox]',
  );
  expect(input, dumpDom()).toBeTruthy();
  input!.dispatchEvent(new window.Event('change', {bubbles: true}));
  h.rerender();
}

function clickNav(h: Harness, dir: 'left' | 'right'): void {
  const nav = container.querySelector('.pf-flamegraph-collection__flame-nav');
  expect(nav, dumpDom()).toBeTruthy();
  const buttons = nav!.querySelectorAll('button');
  const button = dir === 'left' ? buttons[0] : buttons[buttons.length - 1];
  button.dispatchEvent(new window.MouseEvent('click', {bubbles: true}));
  h.rerender();
}

describe('FlamegraphCollection', () => {
  test('merged mode sums the whole working set into one panel', () => {
    const h = renderCollection();
    expect(summaryText()).toBe('Merging 2 of 2 profiles');
    expect(h.metricsForKeys).toHaveBeenCalledWith(['a.pprof', 'b.pprof']);
    expect(panel.last().metrics).toEqual([metricFor(['a.pprof', 'b.pprof'])]);
    // The reconciled flamegraph state was written back to the store.
    expect(h.state().flamegraphState?.selectedMetricId).toBe('cpu (ns)');
  });

  test('metric arrays stay reference-stable across no-op re-renders', () => {
    const h = renderCollection();
    h.rerender();
    h.rerender();
    const metricSets = panel.renders.map((r) => r.metrics);
    expect(metricSets.length).toBeGreaterThan(1);
    for (const metrics of metricSets) {
      expect(metrics).toBe(metricSets[0]);
    }
  });

  test('toggling merge steps through entries one at a time', () => {
    const h = renderCollection();
    toggleMerge(h);
    expect(h.state().merge).toBe(false);
    expect(summaryText()).toBe('Showing 2 of 2 profiles');
    expect(positionText()).toBe('1 / 2');
    expect(h.metricsForKeys).toHaveBeenCalledWith(['a.pprof']);
    expect(h.metricsForKeys).toHaveBeenCalledWith(['b.pprof']);
    expect(
      container.querySelector('.pf-flamegraph-collection__flame-cell-title')
        ?.textContent,
    ).toBe('a.pprof');
    expect(panel.last().metrics).toEqual([metricFor(['a.pprof'])]);

    clickNav(h, 'right');
    expect(positionText()).toBe('2 / 2');
    expect(
      container.querySelector('.pf-flamegraph-collection__flame-cell-title')
        ?.textContent,
    ).toBe('b.pprof');
    expect(panel.last().metrics).toEqual([metricFor(['b.pprof'])]);

    clickNav(h, 'left');
    expect(positionText()).toBe('1 / 2');
  });

  test('grid filters shrink the working set', () => {
    const h = renderCollection();
    h.setState({
      ...h.state(),
      filters: [{field: 'c1', op: '>', value: 50}],
    });
    expect(summaryText()).toBe('Merging 1 of 2 profiles');
    expect(h.metricsForKeys).toHaveBeenLastCalledWith(['a.pprof']);
  });

  test('an empty working set renders an empty state, not a panel', () => {
    const h = renderCollection({
      filters: [{field: 'c1', op: '>', value: 1e9}],
    });
    expect(summaryText()).toBe('Merging 0 of 2 profiles');
    expect(h.metricsForKeys).toHaveBeenCalledWith([]);
    expect(
      container.querySelector('.pf-test-fake-flamegraph-panel'),
    ).toBeNull();
    expect(container.querySelector('.pf-empty-state'), dumpDom()).toBeTruthy();
  });

  test('arrow keys step entries when the collection is visible', () => {
    const h = renderCollection({merge: false});
    const root = container.querySelector<HTMLElement>(
      '.pf-flamegraph-collection',
    );
    expect(root, dumpDom()).toBeTruthy();
    // jsdom has no layout, so offsetParent is always null; pretend the
    // component is visible.
    Object.defineProperty(root!, 'offsetParent', {get: () => document.body});

    expect(positionText()).toBe('1 / 2');
    window.dispatchEvent(
      new window.KeyboardEvent('keydown', {key: 'ArrowRight'}),
    );
    h.rerender();
    expect(positionText()).toBe('2 / 2');
    window.dispatchEvent(
      new window.KeyboardEvent('keydown', {key: 'ArrowLeft'}),
    );
    h.rerender();
    expect(positionText()).toBe('1 / 2');
  });

  test('step-mode titles use renderEntryTitle when provided', () => {
    renderCollection(
      {merge: false},
      {
        renderEntryTitle: (key: string) => m('em', `entry ${key}`),
      },
    );
    expect(
      container.querySelector('.pf-flamegraph-collection__flame-cell-title em')
        ?.textContent,
    ).toBe('entry a.pprof');
  });

  test('swapping the rows identity reloads the collection', () => {
    const h = renderCollection();
    expect(summaryText()).toBe('Merging 2 of 2 profiles');
    const newRows: Row[] = [...ROWS, {c0: 'c.pprof', c1: 7}];
    m.render(
      container,
      m(FlamegraphCollection, {
        ...(h.attrs() as FlamegraphCollectionAttrs),
        rows: newRows,
      }),
    );
    expect(summaryText()).toBe('Merging 3 of 3 profiles');
    expect(h.metricsForKeys).toHaveBeenLastCalledWith([
      'a.pprof',
      'b.pprof',
      'c.pprof',
    ]);
  });
});
