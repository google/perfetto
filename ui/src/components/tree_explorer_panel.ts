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
import {assertUnreachable, ensureExists} from '../base/assert';
import type {Trace} from '../public/trace';
import {EmptyState} from '../widgets/empty_state';
import {Spinner} from '../widgets/spinner';
import {Flamegraph, buildFlamegraphExportString} from '../widgets/flamegraph';
import type {ExportDownloadItem} from '../widgets/export_button';
import {TreeExplorerFilterBar} from '../widgets/tree_explorer_filter_bar';
import {TreeExplorerViewSwitcher} from '../widgets/tree_explorer_view_switcher';
import {
  computeHighlightRegex,
  metricId,
  type TreeExplorerAddableMetric,
  type TreeExplorerData,
  type TreeExplorerState,
  type TreeExplorerView,
} from '../widgets/tree_explorer';
import {
  TreeExplorerFetcher,
  type TreeExplorerFetcherDependency,
  type TreeExplorerQueryMetric,
} from './tree_explorer_fetcher';
import {
  TreeExplorerFlatView,
  TreeExplorerTreeView,
  buildFlatExportString,
} from './tree_explorer_table_views';

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
  readonly dependencies?: ReadonlyArray<TreeExplorerFetcherDependency>;

  // Host-provided downloads shown alongside the built-in exports of the
  // displayed tree, for representations the panel cannot build itself.
  readonly extraDownloadItems?: ReadonlyArray<ExportDownloadItem>;
}

// The batteries-included tree explorer: owns a `TreeExplorerFetcher` (created
// on first render, disposed on unmount or when `trace` / `dependencies`
// identity changes) and composes the view switcher, the shared filter bar
// and the active view (flamegraph canvas, call tree or flat function table)
// of the fetched tree. Lets area-selection tabs and details panels render a
// tree explorer without managing `[Symbol.asyncDispose]` themselves.
//
// Hosts that want the view switcher elsewhere (a DetailsShell header, an
// existing tab strip) compose `TreeExplorerViewSwitcher`,
// `TreeExplorerFilterBar` and the views directly instead of using this panel.
export class TreeExplorerPanel implements m.ClassComponent<TreeExplorerPanelAttrs> {
  private fetcher?: TreeExplorerFetcher;
  private lastTrace?: Trace;
  private lastDeps?: ReadonlyArray<TreeExplorerFetcherDependency>;

  private data?: TreeExplorerData;
  private readonly queryLimiter = new AsyncLimiter();
  private lastAttrs?: TreeExplorerPanelAttrs;
  private monitor = this.createMonitor();
  private highlightPattern = '';

  // Watches everything that affects the fetched tree. Notably absent:
  // displayMode, which only changes how the already-fetched tree is shown —
  // except through effectiveView(), whose result it can change (flat mode
  // always fetches the TOP_DOWN shape).
  private createMonitor(): Monitor {
    return new Monitor([
      () => this.lastAttrs?.metrics,
      () => this.lastAttrs?.state?.filters,
      () => this.lastAttrs?.state?.selectedMetricId,
      () => this.lastAttrs?.state?.addedMetricIds,
      () =>
        this.lastAttrs?.state &&
        JSON.stringify(effectiveView(this.lastAttrs.state)),
    ]);
  }

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
      this.monitor = this.createMonitor();
    }
    const fetcher = this.fetcher;
    const {metrics, state} = attrs;
    if (this.monitor.ifStateChanged()) {
      this.data = undefined;
      if (metrics && state) {
        const fetchState = {...state, view: effectiveView(state)};
        this.queryLimiter.schedule(async () => {
          this.data = undefined;
          this.data = await fetcher.fetch(metrics, fetchState);
        });
      }
    }
    const shownState = state ?? {
      view: {kind: 'TOP_DOWN' as const},
      selectedMetricId: '',
      addedMetricIds: [],
      displayMode: 'flamegraph' as const,
      filters: [],
    };
    const shownMetrics = metrics ?? [];
    const selectedMetric = shownMetrics.find(
      (x) => metricId(x) === shownState.selectedMetricId,
    );
    const highlightRegex = computeHighlightRegex(this.highlightPattern);
    const displayMode = shownState.displayMode;
    return m(
      '.pf-tree-explorer',
      m(TreeExplorerViewSwitcher, {
        state: shownState,
        onStateChange: attrs.onStateChange,
      }),
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
        highlightDisabled: displayMode !== 'flamegraph',
        directionDisabled: displayMode === 'flat',
        onExportData:
          selectedMetric === undefined
            ? undefined
            : async (format) => {
                const data = ensureExists(this.data);
                return displayMode === 'flat'
                  ? buildFlatExportString(data, selectedMetric, format)
                  : buildFlamegraphExportString(data, selectedMetric, format);
              },
        exportFileBaseName:
          displayMode === 'flat'
            ? 'functions'
            : displayMode === 'tree'
              ? 'call_tree'
              : 'flamegraph',
        extraDownloadItems: attrs.extraDownloadItems,
      }),
      this.renderView(attrs, shownState, highlightRegex),
    );
  }

  private renderView(
    attrs: TreeExplorerPanelAttrs,
    state: TreeExplorerState,
    highlightRegex: RegExp | undefined,
  ): m.Children {
    const displayMode = state.displayMode;
    if (displayMode === 'flamegraph') {
      return m(Flamegraph, {
        metrics: attrs.metrics ?? [],
        state,
        data: this.data,
        highlightRegex,
        onStateChange: attrs.onStateChange,
      });
    }
    if (this.data === undefined) {
      return m(
        '.pf-tree-explorer__loading',
        m(
          EmptyState,
          {icon: 'bar_chart', title: 'Computing graph ...'},
          m(Spinner, {easing: true}),
        ),
      );
    }
    const unit =
      (attrs.metrics ?? []).find((x) => metricId(x) === state.selectedMetricId)
        ?.unit ?? '';
    switch (displayMode) {
      case 'tree':
        return m(TreeExplorerTreeView, {data: this.data, unit});
      case 'flat':
        return m(TreeExplorerFlatView, {data: this.data, unit});
      default:
        assertUnreachable(displayMode);
    }
  }

  async onremove(): Promise<void> {
    await this.fetcher?.[Symbol.asyncDispose]();
    this.fetcher = undefined;
  }
}

// The view whose tree shape is actually fetched. The flat function table
// aggregates the callee direction, so direction has no meaning there: fetch
// the TOP_DOWN shape and grey out the direction selector.
function effectiveView(state: TreeExplorerState): TreeExplorerView {
  if (
    state.displayMode === 'flat' &&
    (state.view.kind === 'TOP_DOWN' || state.view.kind === 'BOTTOM_UP')
  ) {
    return {kind: 'TOP_DOWN'};
  }
  return state.view;
}
