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

import {z} from 'zod';

// Which tree the views show when the tree is the merge of several profiles.
// Only has an effect when the host supplies the profiles (see
// TreeExplorerPanelAttrs.merge).
export const TREE_EXPLORER_MERGE_SELECTION_SCHEMA = z
  .object({
    // Whether the views show the merge of all the profiles, or one profile
    // on its own.
    merged: z.boolean(),
    // The profile shown on its own (see TreeExplorerProfile.key). Undefined,
    // or a key no profile has, means the first profile.
    profileKey: z.string().optional(),
  })
  .readonly();

export type TreeExplorerMergeSelection = z.infer<
  typeof TREE_EXPLORER_MERGE_SELECTION_SCHEMA
>;

export const DEFAULT_TREE_EXPLORER_MERGE_SELECTION: TreeExplorerMergeSelection =
  {merged: true};

export function getTreeExplorerMergeSelection(state: {
  readonly merge?: TreeExplorerMergeSelection;
}): TreeExplorerMergeSelection {
  return state.merge ?? DEFAULT_TREE_EXPLORER_MERGE_SELECTION;
}

// One of the profiles a tree merges, e.g. a file of a pprof archive.
export interface TreeExplorerProfile {
  // Identifies the profile in TreeExplorerMergeSelection.profileKey. Should
  // be stable across sessions, as the selection is persisted.
  readonly key: string;
  // Short human-readable name, e.g. the profile's file name.
  readonly label: string;
}

// The index in `profiles` of the profile `selection` shows on its own: the
// one with the selected key, or else the first.
export function shownTreeExplorerProfileIndex(
  profiles: ReadonlyArray<TreeExplorerProfile>,
  selection: TreeExplorerMergeSelection,
): number {
  return Math.max(
    profiles.findIndex((p) => p.key === selection.profileKey),
    0,
  );
}

// Selects the profile `step` places after the one shown on its own (e.g. -1
// for the previous one). Returns undefined when there is no such profile, or
// when the merged tree is shown, as there is nothing to step through then.
export function stepTreeExplorerProfile(
  profiles: ReadonlyArray<TreeExplorerProfile>,
  selection: TreeExplorerMergeSelection,
  step: number,
): TreeExplorerMergeSelection | undefined {
  if (selection.merged) {
    return undefined;
  }
  const index = shownTreeExplorerProfileIndex(profiles, selection) + step;
  if (index < 0 || index >= profiles.length) {
    return undefined;
  }
  return {merged: false, profileKey: profiles[index].key};
}
