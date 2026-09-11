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
import {assertUnreachable, ensureExists} from '../base/assert';
import {EmptyState} from '../widgets/empty_state';
import {Spinner} from '../widgets/spinner';
import {Flamegraph, buildFlamegraphExportString} from '../widgets/flamegraph';
import type {ExportDownloadItem} from '../widgets/export_button';
import {TreeExplorerFilterBar} from '../widgets/tree_explorer_filter_bar';
import {TreeExplorerViewSwitcher} from '../widgets/tree_explorer_view_switcher';
import {
  computeHighlightRegex,
  createDefaultTreeExplorerState,
  metricId,
  type TreeExplorerAddableMetric,
  type TreeExplorerData,
  type TreeExplorerState,
  type TreeExplorerView,
} from '../widgets/tree_explorer';
import type {
  TreeExplorerFetcher,
  TreeExplorerQueryMetric,
} from './tree_explorer_fetcher';
import {
  TreeExplorerFlatView,
  TreeExplorerTreeView,
  buildFlatExportString,
} from './tree_explorer_table_views';

export interface TreeExplorerPanelAttrs {
  // The fetcher supplying the tree, or undefined to show a pending state.
  // Owned by the caller: this panel never creates or disposes it, so that the
  // lifetime of the engine-side resources the metric SQL depends on is decided
  // in one place (where the metrics themselves are created).
  readonly fetcher: TreeExplorerFetcher;

  // Caller-owned tree explorer state (filters, view, selected metric). When
  // metrics change, pass `updateTreeExplorerState(state, metrics)` to keep
  // the selected metric valid. Undefined shows an empty pending state.
  readonly state?: TreeExplorerState;

  readonly onStateChange: (state: TreeExplorerState) => void;

  readonly addableMetrics?: ReadonlyArray<TreeExplorerAddableMetric>;
  readonly onAddMetric?: (metric: TreeExplorerAddableMetric) => void;

  // Host-provided downloads shown alongside the built-in exports of the
  // displayed tree, for representations the panel cannot build itself.
  readonly extraDownloadItems?: ReadonlyArray<ExportDownloadItem>;
}

// The batteries-included tree explorer: composes the view switcher, the shared
// filter bar and the active view (flamegraph canvas, call tree or flat
// function table) of the tree supplied by `attrs.fetcher`.
//
// The panel is stateless with respect to fetching: the fetcher is created by
// the host where the metrics are created, and disposed by that host when the
// metrics are superseded. Hosts that want the view switcher elsewhere (a
// DetailsShell header, an existing tab strip) compose
// `TreeExplorerViewSwitcher`, `TreeExplorerFilterBar` and the views directly
// instead of using this panel.
export class TreeExplorerPanel implements m.ClassComponent<TreeExplorerPanelAttrs> {
  private highlightPattern = '';

  view({attrs}: m.CVnode<TreeExplorerPanelAttrs>): m.Children {
    const {fetcher, state = createDefaultTreeExplorerState(fetcher.metrics)} =
      attrs;
    const metrics = fetcher?.metrics;
    const data =
      fetcher !== undefined && state !== undefined
        ? fetcher.use({...state, view: effectiveView(state)}).data
        : undefined;

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
        data: data,
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
                const dataExists = ensureExists(data);
                return displayMode === 'flat'
                  ? buildFlatExportString(dataExists, selectedMetric, format)
                  : buildFlamegraphExportString(
                      dataExists,
                      selectedMetric,
                      format,
                    );
              },
        exportFileBaseName:
          displayMode === 'flat'
            ? 'functions'
            : displayMode === 'tree'
              ? 'call_tree'
              : 'flamegraph',
        extraDownloadItems: attrs.extraDownloadItems,
      }),
      this.renderView(attrs, shownMetrics, shownState, highlightRegex, data),
    );
  }

  private renderView(
    attrs: TreeExplorerPanelAttrs,
    metrics: ReadonlyArray<TreeExplorerQueryMetric>,
    state: TreeExplorerState,
    highlightRegex: RegExp | undefined,
    data: TreeExplorerData | undefined,
  ): m.Children {
    const displayMode = state.displayMode;
    if (displayMode === 'flamegraph') {
      return m(Flamegraph, {
        metrics,
        state,
        data,
        highlightRegex,
        onStateChange: attrs.onStateChange,
      });
    }
    if (data === undefined) {
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
      metrics.find((x) => metricId(x) === state.selectedMetricId)?.unit ?? '';
    switch (displayMode) {
      case 'tree':
        return m(TreeExplorerTreeView, {data, unit});
      case 'flat':
        return m(TreeExplorerFlatView, {data, unit});
      default:
        assertUnreachable(displayMode);
    }
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
