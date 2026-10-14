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
import {
  type TreeExplorerState,
  updateTreeExplorerState,
} from '../widgets/tree_explorer';
import {
  type TreeExplorerMergeSelection,
  type TreeExplorerProfile,
  getTreeExplorerMergeSelection,
  shownTreeExplorerProfileIndex,
} from '../widgets/tree_explorer_merge';
import {TreeExplorerMergeControls} from '../widgets/tree_explorer_merge_controls';
import type {TreeExplorerFetcher} from './tree_explorer_fetcher';

// The profiles a tree merges, e.g. the files of a pprof archive.
export interface TreeExplorerMerge {
  // The profiles, in the order the views step through them. The panel's
  // tree (TreeExplorerPanelAttrs.fetcher) must be their merge, computed by
  // the caller, e.g. in SQL by summing the samples of all of them.
  readonly profiles: ReadonlyArray<TreeExplorerProfile>;

  // Supplies the tree of the profile with `key` on its own, fetched with the
  // same state (filters, view) as the merged tree, so its metrics should
  // have the same ids as the merged tree's, though it may lack some. Owned
  // by the caller like TreeExplorerPanelAttrs.fetcher, and only called on
  // render for the shown profile, so the caller can keep just that one
  // alive (e.g. with a Memo).
  readonly profileFetcher: (key: string) => TreeExplorerFetcher;
}

// The profiles the panel's tree merges, if there are any to choose between.
function mergeOf(
  attrsMerge?: TreeExplorerMerge,
): TreeExplorerMerge | undefined {
  return attrsMerge !== undefined && attrsMerge.profiles.length > 1
    ? attrsMerge
    : undefined;
}

export interface ResolvedTreeExplorerMerge {
  readonly merge?: TreeExplorerMerge;
  readonly mergeSelection: TreeExplorerMergeSelection;
  readonly fetcher: TreeExplorerFetcher;
  readonly state: TreeExplorerState;
  readonly onStateChange: (state: TreeExplorerState) => void;
}

// Which tree the panel shows given `state.merge`, and the state the views
// get for it.
export function resolveTreeExplorerMerge(
  attrsFetcher: TreeExplorerFetcher,
  hostState: TreeExplorerState,
  onHostStateChange: (state: TreeExplorerState) => void,
  mergeAttr?: TreeExplorerMerge,
): ResolvedTreeExplorerMerge {
  const merge = mergeOf(mergeAttr);
  const mergeSelection = getTreeExplorerMergeSelection(hostState);
  const fetcher =
    merge === undefined || mergeSelection.merged
      ? attrsFetcher
      : merge.profileFetcher(
          merge.profiles[
            shownTreeExplorerProfileIndex(merge.profiles, mergeSelection)
          ].key,
        );
  // A profile may lack the metric selected for the merged tree: it then
  // shows its first metric, leaving the caller's selection as it is for
  // the other trees.
  const state = updateTreeExplorerState(hostState, fetcher.metrics);
  // The views are given `state`, so they report the metric shown instead
  // of the one selected: keep the selection unless they changed it.
  const onStateChange = (newState: TreeExplorerState) =>
    onHostStateChange(
      state !== hostState &&
        newState.selectedMetricId === state.selectedMetricId
        ? {...newState, selectedMetricId: hostState.selectedMetricId}
        : newState,
    );
  return {merge, mergeSelection, fetcher, state, onStateChange};
}

export function renderTreeExplorerMergeControls(
  merge: TreeExplorerMerge | undefined,
  mergeSelection: TreeExplorerMergeSelection,
  onSelectionChange: (selection: TreeExplorerMergeSelection) => void,
): m.Children {
  if (merge === undefined) {
    return null;
  }
  return m(TreeExplorerMergeControls, {
    profiles: merge.profiles,
    selection: mergeSelection,
    onSelectionChange,
  });
}

export function exportMergeSuffix(
  merge: TreeExplorerMerge | undefined,
  mergeSelection: TreeExplorerMergeSelection,
): string {
  return merge !== undefined && mergeSelection.merged ? '_merged' : '';
}
