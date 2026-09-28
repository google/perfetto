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

import './tree_explorer_merge_controls.scss';
import m from 'mithril';
import {assertIsInstance} from '../base/assert';
import {Icons} from '../base/semantic_icons';
import {Button} from './button';
import {CopyToClipboardButton} from './copy_to_clipboard_button';
import {Select} from './select';
import {Switch} from './switch';
import {
  shownTreeExplorerProfileIndex,
  stepTreeExplorerProfile,
  type TreeExplorerMergeSelection,
  type TreeExplorerProfile,
} from './tree_explorer_merge';

export interface TreeExplorerMergeControlsAttrs {
  // The profiles the tree merges, in the order they are stepped through. At
  // least two: there is nothing to choose between otherwise.
  readonly profiles: ReadonlyArray<TreeExplorerProfile>;

  readonly selection: TreeExplorerMergeSelection;
  readonly onSelectionChange: (selection: TreeExplorerMergeSelection) => void;
}

// Chooses what a tree explorer whose tree merges several profiles shows: the
// merged tree, or one profile at a time, picked from a list or stepped
// through in order. The counterpart of diffing against a baseline.
export class TreeExplorerMergeControls implements m.ClassComponent<TreeExplorerMergeControlsAttrs> {
  view({attrs}: m.CVnode<TreeExplorerMergeControlsAttrs>): m.Children {
    const {profiles, selection, onSelectionChange} = attrs;
    const count = profiles.length;
    const index = shownTreeExplorerProfileIndex(profiles, selection);
    const shown = profiles[index];
    const step = (by: number) => {
      const next = stepTreeExplorerProfile(profiles, selection, by);
      if (next !== undefined) {
        onSelectionChange(next);
      }
    };
    return m(
      '.pf-tree-explorer-merge-controls',
      m(Switch, {
        label: 'Merge',
        checked: selection.merged,
        title: selection.merged
          ? `Show the ${count} profiles one at a time`
          : `Show the ${count} profiles merged into one`,
        onchange: () =>
          onSelectionChange({...selection, merged: !selection.merged}),
      }),
      selection.merged
        ? m('.pf-tree-explorer-merge-controls__summary', `${count} profiles`)
        : [
            m(
              Select,
              {
                className: 'pf-tree-explorer-merge-controls__select',
                title: shown.label,
                onchange: (e: Event) => {
                  assertIsInstance(e.target, HTMLSelectElement);
                  onSelectionChange({
                    merged: false,
                    profileKey: e.target.value,
                  });
                },
              },
              profiles.map((profile) =>
                m(
                  'option',
                  {value: profile.key, selected: profile.key === shown.key},
                  profile.label,
                ),
              ),
            ),
            // The name is only in <option> text, which the mouse cannot
            // select.
            m(CopyToClipboardButton, {
              textToCopy: shown.label,
              title: 'Copy profile name',
              compact: true,
            }),
            m(
              '.pf-tree-explorer-merge-controls__stepper',
              m(Button, {
                icon: Icons.PrevPage,
                compact: true,
                disabled: index <= 0,
                title: 'Previous profile',
                onclick: () => step(-1),
              }),
              m(
                '.pf-tree-explorer-merge-controls__summary',
                `${index + 1} / ${count}`,
              ),
              m(Button, {
                icon: Icons.NextPage,
                compact: true,
                disabled: index >= count - 1,
                title: 'Next profile',
                onclick: () => step(1),
              }),
            ),
          ],
    );
  }
}
