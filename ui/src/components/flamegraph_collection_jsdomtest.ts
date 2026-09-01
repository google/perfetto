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

import type {Trace} from '../public/trace';
import type {Row} from '../trace_processor/query_result';
import type {TreeExplorerQueryMetric} from './tree_explorer_fetcher';
import {
  DEFAULT_FLAMEGRAPH_COLLECTION_STATE,
  FlamegraphCollection,
} from './flamegraph_collection';
import type {
  FlamegraphCollectionAttrs,
  FlamegraphCollectionState,
} from './flamegraph_collection';

// What the collection hands its panel is invisible to the DOM, so these stub
// TreeExplorerPanel and assert on its attrs. Anything visible is covered in a
// real browser by aggregate_profiles_merge.test.ts.
const panel = vi.hoisted(() => ({
  renders: [] as Array<{metrics?: unknown}>,
  last() {
    return this.renders[this.renders.length - 1];
  },
}));

vi.mock('./tree_explorer_panel', () => ({
  TreeExplorerPanel: {
    view: ({attrs}: m.CVnode<{metrics?: unknown}>) => {
      panel.renders.push({metrics: attrs.metrics});
      return m('.pf-test-fake-flamegraph-panel');
    },
  },
}));

const ROWS: Row[] = [
  {c0: 'a.pprof', c1: 100},
  {c0: 'b.pprof', c1: 25},
];

const COLUMNS = [
  {field: 'c0', title: 'profile', kind: 'id'},
  {field: 'c1', title: 'cpu (ns)', kind: 'numeric', unit: 'ns'},
] as const;

const metricFor = (keys: ReadonlyArray<string>): TreeExplorerQueryMetric => ({
  name: 'cpu (ns)',
  unit: 'ns',
  statement: `select ... where key in (${keys.join(',')})`,
});

let container: HTMLElement;

beforeEach(() => {
  container = document.createElement('div');
  document.body.appendChild(container);
  panel.renders.length = 0;
});

afterEach(() => {
  m.render(container, null);
  container.remove();
});

interface Harness {
  rerender: () => void;
  metricsForKeys: ReturnType<typeof vi.fn>;
}

// onStateChange stores the state for the next explicit rerender, as a real
// caller would. Fresh vnodes each time: m.render no-ops on an identical one.
function renderCollection(
  initial?: Partial<FlamegraphCollectionState>,
): Harness {
  let state = {...DEFAULT_FLAMEGRAPH_COLLECTION_STATE, ...initial};
  const metricsForKeys = vi.fn((keys: ReadonlyArray<string>) =>
    keys.length > 0 ? [metricFor(keys)] : [],
  );
  const rerender = () =>
    m.render(
      container,
      m(FlamegraphCollection, {
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
      } satisfies FlamegraphCollectionAttrs),
    );
  rerender();
  // Flush the reconciled flamegraph state written back on first rebuild.
  rerender();
  return {rerender, metricsForKeys};
}

describe('FlamegraphCollection', () => {
  test('merged mode sums the whole working set into one panel', () => {
    const h = renderCollection();
    expect(h.metricsForKeys).toHaveBeenCalledWith(['a.pprof', 'b.pprof']);
    expect(panel.last().metrics).toEqual([metricFor(['a.pprof', 'b.pprof'])]);
  });

  test('metric arrays stay reference-stable across no-op re-renders', () => {
    // TreeExplorerPanel monitors `metrics` by identity: a fresh array per
    // render would re-run the query on every redraw.
    const h = renderCollection();
    h.rerender();
    h.rerender();
    const sets = panel.renders.map((r) => r.metrics);
    expect(sets.length).toBeGreaterThan(1);
    for (const metrics of sets) {
      expect(metrics).toBe(sets[0]);
    }
  });

  test('step mode asks for one entry at a time', () => {
    const h = renderCollection({merge: false});
    expect(h.metricsForKeys).toHaveBeenCalledWith(['a.pprof']);
    expect(h.metricsForKeys).toHaveBeenCalledWith(['b.pprof']);
    expect(panel.last().metrics).toEqual([metricFor(['a.pprof'])]);
  });

  test('grid filters shrink the working set', () => {
    const h = renderCollection({filters: [{field: 'c1', op: '>', value: 50}]});
    expect(h.metricsForKeys).toHaveBeenLastCalledWith(['a.pprof']);
  });

  test('an empty working set renders an empty state, not a panel', () => {
    // Reconciling a state against an empty metric set throws.
    const h = renderCollection({filters: [{field: 'c1', op: '>', value: 1e9}]});
    expect(h.metricsForKeys).toHaveBeenCalledWith([]);
    expect(
      container.querySelector('.pf-test-fake-flamegraph-panel'),
    ).toBeNull();
    expect(container.querySelector('.pf-empty-state')).toBeTruthy();
  });
});
