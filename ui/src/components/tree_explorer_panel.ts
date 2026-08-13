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

import './tree_explorer_panel.scss';
import m from 'mithril';
import {AsyncLimiter} from '../base/async_limiter';
import {Monitor} from '../base/monitor';
import {ensureExists} from '../base/assert';
import type {Trace} from '../public/trace';
import {Flamegraph, buildFlamegraphExportString} from '../widgets/flamegraph';
import {TreeExplorerFilterBar} from '../widgets/tree_explorer_filter_bar';
import {
  computeHighlightRegex,
  metricId,
  type TreeExplorerAddableMetric,
  type TreeExplorerData,
  type TreeExplorerState,
} from '../widgets/tree_explorer';
import {
  TreeExplorerFetcher,
  type TreeExplorerQueryMetric,
} from './tree_explorer_fetcher';

export interface TreeExplorerPanelAttrs {
  readonly trace: Trace;

  // The metrics to render. Undefined shows a loading state.
  readonly metrics?: ReadonlyArray<TreeExplorerQueryMetric>;

  // Caller-owned tree explorer state (filters, view, selected metric). When
  // `metrics` change, pass `updateTreeExplorerState(state, metrics)` to keep
  // the selected metric valid. Undefined shows an empty pending state.
  readonly state?: TreeExplorerState;

  readonly onStateChange: (state: TreeExplorerState) => void;

  readonly addableMetrics?: ReadonlyArray<TreeExplorerAddableMetric>;
  readonly onAddMetric?: (metric: TreeExplorerAddableMetric) => void;

  // Perfetto tables / indices the metric SQL depends on. The panel forwards
  // them to the inner `TreeExplorerFetcher`, which disposes them along with
  // itself on unmount or when the array reference changes.
  readonly dependencies?: ReadonlyArray<AsyncDisposable>;
}

// The batteries-included tree explorer: owns a `TreeExplorerFetcher` (created
// on first render, disposed on unmount or when `trace` / `dependencies`
// identity changes) and composes the shared filter bar with the flamegraph
// view of the fetched tree. Lets area-selection tabs and details panels
// render a tree explorer without managing `[Symbol.asyncDispose]` themselves.
export class TreeExplorerPanel implements m.ClassComponent<TreeExplorerPanelAttrs> {
  private fetcher?: TreeExplorerFetcher;
  private lastTrace?: Trace;
  private lastDeps?: ReadonlyArray<AsyncDisposable>;

  private data?: TreeExplorerData;
  private readonly queryLimiter = new AsyncLimiter();
  private lastAttrs?: TreeExplorerPanelAttrs;
  private monitor = new Monitor([
    () => this.lastAttrs?.metrics,
    () => this.lastAttrs?.state,
  ]);
  private highlightPattern = '';

  view({attrs}: m.CVnode<TreeExplorerPanelAttrs>): m.Children {
    this.lastAttrs = attrs;
    if (
      this.fetcher === undefined ||
      this.lastTrace !== attrs.trace ||
      this.lastDeps !== attrs.dependencies
    ) {
      void this.fetcher?.[Symbol.asyncDispose]();
      this.fetcher = new TreeExplorerFetcher(attrs.trace, attrs.dependencies);
      this.lastTrace = attrs.trace;
      this.lastDeps = attrs.dependencies;
      // A new fetcher has no tables yet: force a refetch even if the
      // metrics/state references are unchanged.
      this.monitor = new Monitor([
        () => this.lastAttrs?.metrics,
        () => this.lastAttrs?.state,
      ]);
    }
    const fetcher = this.fetcher;
    const {metrics, state} = attrs;
    if (this.monitor.ifStateChanged()) {
      this.data = undefined;
      if (metrics && state) {
        this.queryLimiter.schedule(async () => {
          this.data = undefined;
          this.data = await fetcher.fetch(metrics, state);
        });
      }
    }
    const shownState = state ?? {
      view: {kind: 'TOP_DOWN' as const},
      selectedMetricId: '',
      addedMetricIds: [],
      filters: [],
    };
    const shownMetrics = metrics ?? [];
    const selectedMetric = shownMetrics.find(
      (x) => metricId(x) === shownState.selectedMetricId,
    );
    const highlightRegex = computeHighlightRegex(this.highlightPattern);
    return m(
      '.pf-tree-explorer',
      m(TreeExplorerFilterBar, {
        metrics: shownMetrics,
        state: shownState,
        data: this.data,
        onStateChange: attrs.onStateChange,
        addableMetrics: attrs.addableMetrics,
        onAddMetric: attrs.onAddMetric,
        highlightPattern: this.highlightPattern,
        highlightRegex,
        onHighlightChange: (pattern) => {
          this.highlightPattern = pattern;
        },
        onExportData:
          selectedMetric === undefined
            ? undefined
            : async (format) =>
                buildFlamegraphExportString(
                  ensureExists(this.data),
                  selectedMetric,
                  format,
                ),
        exportFileBaseName: 'flamegraph',
      }),
      m(Flamegraph, {
        metrics: shownMetrics,
        state: shownState,
        data: this.data,
        highlightRegex,
        onStateChange: attrs.onStateChange,
      }),
    );
  }

  async onremove(): Promise<void> {
    await this.fetcher?.[Symbol.asyncDispose]();
    this.fetcher = undefined;
  }
}
