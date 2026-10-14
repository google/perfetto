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
import {afterEach, beforeEach, describe, expect, test} from 'vitest';
import {prettyDOM} from '@testing-library/dom';
import type {AsyncMemoResult} from '../base/async_memo';
import {
  createDefaultTreeExplorerState,
  type TreeExplorerData,
  type TreeExplorerState,
} from '../widgets/tree_explorer';
import type {TreeExplorerFetcher} from './tree_explorer_fetcher';
import {TreeExplorerPanel, type TreeExplorerMerge} from './tree_explorer_panel';

// DOM tests for which tree TreeExplorerPanel shows given the profiles its
// tree merges: the merged tree (the host's fetcher) or one profile's. The
// fetchers are fakes recording the states they are asked for and never
// resolving, so no view needs a canvas.

interface FakeFetcher {
  readonly fetcher: TreeExplorerFetcher;
  // The states the panel fetched with, one per render.
  readonly uses: TreeExplorerState[];
}

function fakeFetcher(metricNames: ReadonlyArray<string>): FakeFetcher {
  const uses: TreeExplorerState[] = [];
  const fake = {
    metrics: metricNames.map((name) => ({name, unit: 'count', statement: ''})),
    use: (state: TreeExplorerState): AsyncMemoResult<TreeExplorerData> => {
      uses.push(state);
      return {isPending: true};
    },
  };
  return {fetcher: fake as unknown as TreeExplorerFetcher, uses};
}

const PROFILES = [
  {key: 'a', label: 'a.pprof'},
  {key: 'b', label: 'b.pprof'},
  {key: 'c', label: 'c.pprof'},
];

let container: HTMLElement;

beforeEach(() => {
  container = document.createElement('div');
  document.body.appendChild(container);
});

afterEach(() => {
  m.render(container, null);
  container.remove();
});

function dumpDom(): string {
  const out = prettyDOM(container);
  return typeof out === 'string' ? out : '';
}

// Renders a panel whose merged tree has the 'allocations' and 'cpu' metrics,
// and whose profile 'a' has 'cpu' and 'wall' but not 'allocations'.
function renderPanel(opts: {
  readonly state?: TreeExplorerState;
  readonly profiles?: TreeExplorerMerge['profiles'];
  readonly noMerge?: boolean;
}) {
  const merged = fakeFetcher(['allocations', 'cpu']);
  const byKey = new Map<string, FakeFetcher>([
    ['a', fakeFetcher(['cpu', 'wall'])],
    ['b', fakeFetcher(['allocations', 'cpu'])],
    ['c', fakeFetcher(['allocations', 'cpu'])],
  ]);
  const requestedKeys: string[] = [];
  const stateChanges: TreeExplorerState[] = [];
  let state = opts.state;
  // A fresh vnode each time: m.render skips diffing the same vnode.
  const rerender = () =>
    m.render(
      container,
      m(TreeExplorerPanel, {
        fetcher: merged.fetcher,
        state,
        onStateChange: (s) => {
          stateChanges.push(s);
          state = s;
          rerender();
        },
        merge: opts.noMerge
          ? undefined
          : {
              profiles: opts.profiles ?? PROFILES,
              profileFetcher: (key) => {
                requestedKeys.push(key);
                return byKey.get(key)!.fetcher;
              },
            },
      }),
    );
  rerender();
  // The first element matching `selector` whose text starts with `text`.
  const find = (root: ParentNode, selector: string, text: string) => {
    const el = Array.from(root.querySelectorAll<HTMLElement>(selector)).find(
      (e) => e.textContent?.trim().startsWith(text),
    );
    expect(el, `${selector} "${text}" in ${dumpDom()}`).toBeDefined();
    return el!;
  };
  return {
    merged,
    profile: (key: string) => byKey.get(key)!,
    requestedKeys,
    stateChanges,
    hasControls: () =>
      container.querySelector('.pf-tree-explorer-merge-controls') !== null,
    click: (selector: string) => {
      const el = container.querySelector<HTMLElement>(selector);
      expect(el, `${selector} in ${dumpDom()}`).not.toBeNull();
      el!.click();
    },
    // Tabs switch on pointerdown, not click.
    switchTab: (title: string) => {
      find(container, '.pf-tabs__tab', title).dispatchEvent(
        new Event('pointerdown', {bubbles: true}),
      );
    },
    // Picks `metric` in the menu of the filter bar's measure button, which
    // is labeled with the metric `shown`. Tabs shown before stay mounted but
    // hidden, so only the open one's is used. The menu mounts in a portal
    // on document.body once rendered.
    selectMetric: (shown: string, metric: string) => {
      find(
        container,
        '[data-gate-open="true"] .pf-tree-explorer-filter-bar button',
        shown,
      ).click();
      rerender();
      find(document.body, '.pf-menu-item', metric).click();
    },
  };
}

const lastUse = (fake: FakeFetcher) => fake.uses[fake.uses.length - 1];

describe('TreeExplorerPanel merging', () => {
  test('without profiles, shows the tree and no merge controls', () => {
    const p = renderPanel({noMerge: true});
    expect(p.hasControls()).toBe(false);
    expect(p.merged.uses.length).toBe(1);
  });

  test('with a single profile, there is nothing to merge', () => {
    const p = renderPanel({profiles: [PROFILES[0]]});
    expect(p.hasControls()).toBe(false);
    expect(p.merged.uses.length).toBe(1);
    expect(p.requestedKeys).toEqual([]);
  });

  test('shows the merged tree by default', () => {
    const p = renderPanel({});
    expect(p.hasControls()).toBe(true);
    expect(p.merged.uses.length).toBe(1);
    expect(p.requestedKeys).toEqual([]);
    expect(container.textContent).toContain('3 profiles');
  });

  test('turning merging off shows the first profile only', () => {
    const p = renderPanel({});
    p.click('.pf-tree-explorer-merge-controls input[type=checkbox]');
    expect(p.stateChanges.map((s) => s.merge)).toEqual([{merged: false}]);
    expect(p.requestedKeys).toEqual(['a']);
    expect(p.profile('a').uses.length).toBe(1);
    // The merged tree is not fetched while a profile is shown.
    expect(p.merged.uses.length).toBe(1);
  });

  test('shows the selected profile', () => {
    const p = renderPanel({
      state: {
        ...createDefaultTreeExplorerState([{name: 'cpu', unit: 'count'}]),
        merge: {merged: false, profileKey: 'b'},
      },
    });
    expect(p.requestedKeys).toEqual(['b']);
    expect(lastUse(p.profile('b')).selectedMetricId).toBe('cpu');
    expect(p.merged.uses.length).toBe(0);
  });

  test('a profile lacking the selected metric shows its first one', () => {
    const p = renderPanel({
      state: {
        ...createDefaultTreeExplorerState([
          {name: 'allocations', unit: 'count'},
        ]),
        merge: {merged: false, profileKey: 'a'},
      },
    });
    expect(lastUse(p.profile('a')).selectedMetricId).toBe('cpu');

    // Stepping on keeps the host's selection, which the next profile has.
    p.click('button[title="Next profile"]');
    expect(p.stateChanges.length).toBe(1);
    expect(p.stateChanges[0].selectedMetricId).toBe('allocations');
    expect(p.stateChanges[0].merge).toEqual({merged: false, profileKey: 'b'});
    expect(lastUse(p.profile('b')).selectedMetricId).toBe('allocations');
  });

  test('the views keep the selected metric a profile lacks', () => {
    const p = renderPanel({
      state: {
        ...createDefaultTreeExplorerState([
          {name: 'allocations', unit: 'count'},
        ]),
        merge: {merged: false, profileKey: 'a'},
      },
    });

    // Switching tabs while 'a' shows its first metric instead.
    p.switchTab('Call Tree');
    expect(p.stateChanges.length).toBe(1);
    expect(p.stateChanges[0].displayMode).toBe('tree');
    expect(p.stateChanges[0].selectedMetricId).toBe('allocations');
    expect(lastUse(p.profile('a')).selectedMetricId).toBe('cpu');

    // Unlike picking another metric.
    p.selectMetric('cpu', 'wall');
    expect(p.stateChanges.length).toBe(2);
    expect(p.stateChanges[1].selectedMetricId).toBe('wall');
    expect(p.stateChanges[1].displayMode).toBe('tree');
    expect(lastUse(p.profile('a')).selectedMetricId).toBe('wall');
  });
});
