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
import {assertUnreachable} from '../base/assert';
import {
  type TreeExplorerComparison,
  type TreeExplorerData,
  type TreeExplorerDiffWidth,
  type TreeExplorerDisplayMode,
  type TreeExplorerMetric,
  type TreeExplorerState,
  type TreeExplorerView,
  getTreeExplorerComparison,
  metricId,
} from '../widgets/tree_explorer';
import {buildTreeExplorerDiff} from '../widgets/tree_explorer_diff';
import {TreeExplorerDiffControls} from '../widgets/tree_explorer_diff_controls';
import type {TreeExplorerFetcher} from './tree_explorer_fetcher';

// A tree to compare the panel's tree against. For now, only trees of the
// same trace are supported, e.g. another heap dump in it.
export interface TreeExplorerBaseline {
  // Supplies the baseline tree, fetched with the same state (metric, filters,
  // view) as the current tree, so its metrics should have the same ids and
  // properties as the current fetcher's. Owned by the caller, like
  // TreeExplorerPanelAttrs.fetcher.
  readonly fetcher: TreeExplorerFetcher;

  // Short human-readable names of the baseline and the current tree, e.g. of
  // the heap dumps they were read from. Default to 'Baseline' and 'Current'.
  readonly baselineLabel?: string;
  readonly currentLabel?: string;
}

// The panel's diffing state: which tree it shows given a baseline, and the
// last diff built.
export class TreeExplorerPanelDiff {
  private lastDiff?: {
    readonly current: TreeExplorerData;
    readonly baseline: TreeExplorerData;
    readonly width: TreeExplorerDiffWidth;
    readonly diff: TreeExplorerData;
  };

  // The tree the views show, or undefined while it is pending: the fetched
  // tree, or given a baseline, per state.comparison, its diff against the
  // baseline tree or either tree on its own. Also says why the trees cannot
  // be compared, if they cannot: the current tree is then shown.
  shownData(
    fetcher: TreeExplorerFetcher | undefined,
    baseline: TreeExplorerBaseline | undefined,
    state: TreeExplorerState,
    effectiveView: (state: TreeExplorerState) => TreeExplorerView,
  ): {
    readonly data?: TreeExplorerData;
    readonly shown?: TreeExplorerComparison['show'];
    readonly compareDisabledReason?: string;
  } {
    if (fetcher === undefined) {
      return {};
    }
    const fetchState = {...state, view: effectiveView(state)};
    const current = fetcher.use(fetchState).data;
    if (baseline === undefined) {
      return {data: current};
    }
    const selected = (x: TreeExplorerMetric) =>
      metricId(x) === state.selectedMetricId;
    if (!baseline.fetcher.metrics.some(selected)) {
      const name = fetcher.metrics.find(selected)?.name ?? 'such';
      return {
        data: current,
        compareDisabledReason: `The baseline has no ${name} data`,
      };
    }
    // Fetched whatever is shown, so that switching between the trees and
    // their diff is instant.
    const base = baseline.fetcher.use(fetchState).data;
    const {show, width} = getTreeExplorerComparison(state);
    switch (show) {
      case 'CURRENT':
        return {data: current, shown: show};
      case 'BASELINE':
        return {data: base, shown: show};
      case 'DIFF':
        return {
          data:
            current === undefined || base === undefined
              ? undefined
              : this.diffOf(current, base, width),
          shown: show,
        };
      default:
        assertUnreachable(show);
    }
  }

  // Builds the diff only when an input changes: the flamegraph resets its
  // zoom on new data.
  private diffOf(
    current: TreeExplorerData,
    baseline: TreeExplorerData,
    width: TreeExplorerDiffWidth,
  ): TreeExplorerData {
    const last = this.lastDiff;
    if (
      last !== undefined &&
      last.current === current &&
      last.baseline === baseline &&
      last.width === width
    ) {
      return last.diff;
    }
    const diff = buildTreeExplorerDiff(current, baseline, width);
    this.lastDiff = {current, baseline, width, diff};
    return diff;
  }
}

export function renderTreeExplorerDiffControls(
  baseline: TreeExplorerBaseline | undefined,
  state: TreeExplorerState,
  onStateChange: (state: TreeExplorerState) => void,
  displayMode: TreeExplorerDisplayMode,
  disabledReason?: string,
): m.Children {
  if (baseline === undefined) {
    return null;
  }
  return m(TreeExplorerDiffControls, {
    comparison: getTreeExplorerComparison(state),
    onComparisonChange: (comparison) => onStateChange({...state, comparison}),
    baselineLabel: baseline.baselineLabel ?? 'Baseline',
    currentLabel: baseline.currentLabel ?? 'Current',
    flamegraphOptions: displayMode === 'flamegraph',
    disabledReason,
  });
}

export function exportDiffSuffix(
  shown?: TreeExplorerComparison['show'],
): string {
  if (shown === 'DIFF') {
    return '_diff';
  }
  if (shown === 'BASELINE') {
    return '_baseline';
  }
  return '';
}
