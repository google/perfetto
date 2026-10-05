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

import './tree_explorer_diff_controls.scss';
import m from 'mithril';
import {Icons} from '../base/semantic_icons';
import {Button} from './button';
import {MenuDivider, MenuItem, MenuTitle, PopupMenu} from './menu';
import {PopupPosition} from './popup';
import {RadioGroup} from './radio_group';
import {Switch} from './switch';
import type {
  TreeExplorerComparison,
  TreeExplorerDiffColor,
  TreeExplorerDiffWidth,
} from './tree_explorer';
import {treeExplorerDiffColor} from './tree_explorer_diff';

export interface TreeExplorerDiffControlsAttrs {
  readonly comparison: TreeExplorerComparison;
  readonly onComparisonChange: (comparison: TreeExplorerComparison) => void;

  // Short human-readable names of the two trees, e.g. of the heap dumps they
  // were read from.
  readonly baselineLabel: string;
  readonly currentLabel: string;

  // Whether to offer the options which only apply to the flamegraph: node
  // widths and colors.
  readonly flamegraphOptions: boolean;

  // Set when the trees cannot be compared (e.g. the baseline lacks the
  // selected metric): disables the controls, which then show the current
  // tree, and says why.
  readonly disabledReason?: string;
}

const WIDTH_OPTIONS: ReadonlyArray<{
  readonly value: TreeExplorerDiffWidth;
  readonly label: string;
}> = [
  {value: 'COMBINED', label: 'Baseline + current'},
  {value: 'CURRENT', label: 'Current'},
  {value: 'BASELINE', label: 'Baseline'},
];

const COLOR_OPTIONS: ReadonlyArray<{
  readonly value: TreeExplorerDiffColor;
  readonly label: string;
}> = [
  {value: 'ABSOLUTE', label: 'Absolute change'},
  {value: 'RELATIVE', label: 'Relative change'},
];

const COLOR_DESCRIPTIONS: Record<TreeExplorerDiffColor, string> = {
  ABSOLUTE: 'Node colors show the change relative to the largest change.',
  RELATIVE:
    'Node colors show the change relative to the baseline value, ' +
    'saturating at -100% and +100%.',
};

// Chooses what a tree explorer with a baseline tree shows: the diff of the
// current tree against the baseline (see TreeExplorerComparison), or either
// tree on its own, and how the diff is drawn. The counterpart of switching
// between merged and individual profiles.
export class TreeExplorerDiffControls implements m.ClassComponent<TreeExplorerDiffControlsAttrs> {
  view({attrs}: m.CVnode<TreeExplorerDiffControlsAttrs>): m.Children {
    const {comparison, baselineLabel, currentLabel, disabledReason} = attrs;
    const disabled = disabledReason !== undefined;
    const isDiff = comparison.show === 'DIFF' && !disabled;
    const update = (change: Partial<TreeExplorerComparison>) =>
      attrs.onComparisonChange({...comparison, ...change});
    return m(
      '.pf-tree-explorer-diff-controls',
      m(Switch, {
        label: 'Diff',
        checked: isDiff,
        disabled,
        title:
          disabledReason ??
          `Show how ${currentLabel} changed from ${baselineLabel}`,
        onchange: () => update({show: isDiff ? 'CURRENT' : 'DIFF'}),
      }),
      !isDiff &&
        m(
          RadioGroup,
          {
            selectedValue: disabled ? 'CURRENT' : comparison.show,
            disabled,
            onValueChange: (show) =>
              update({show: show === 'BASELINE' ? 'BASELINE' : 'CURRENT'}),
          },
          m(
            RadioGroup.Button,
            {value: 'BASELINE', title: baselineLabel},
            'Baseline',
          ),
          m(
            RadioGroup.Button,
            {value: 'CURRENT', title: currentLabel},
            'Current',
          ),
        ),
      isDiff &&
        attrs.flamegraphOptions && [
          m(
            '.pf-tree-explorer-diff-controls__legend',
            {title: COLOR_DESCRIPTIONS[comparison.color]},
            'Shrank',
            m('canvas.pf-tree-explorer-diff-controls__scale', {
              width: LEGEND_STEPS,
              height: 1,
              oncreate: ({dom}) => paintLegend(dom as HTMLCanvasElement),
            }),
            'Grew',
          ),
          m(
            PopupMenu,
            {
              trigger: m(Button, {
                icon: 'tune',
                compact: true,
                title: 'Diff options',
              }),
              position: PopupPosition.BottomEnd,
            },
            m(MenuTitle, {label: 'Node width'}),
            WIDTH_OPTIONS.map(({value, label}) =>
              m(MenuItem, {
                label,
                icon:
                  comparison.width === value
                    ? Icons.RadioChecked
                    : Icons.RadioUnchecked,
                onclick: () => update({width: value}),
              }),
            ),
            m(MenuDivider),
            m(MenuTitle, {label: 'Node color'}),
            COLOR_OPTIONS.map(({value, label}) =>
              m(MenuItem, {
                label,
                icon:
                  comparison.color === value
                    ? Icons.RadioChecked
                    : Icons.RadioUnchecked,
                onclick: () => update({color: value}),
              }),
            ),
          ),
        ],
    );
  }
}

// The legend is the diff palette painted on a canvas (stretched by CSS),
// from the most shrunk to the most grown, so that it shares its colors with
// the flamegraph without duplicating them in styles.
const LEGEND_STEPS = 32;

function paintLegend(canvas: HTMLCanvasElement) {
  const ctx = canvas.getContext('2d');
  if (ctx === null) {
    return;
  }
  for (let i = 0; i < LEGEND_STEPS; i++) {
    const score = (2 * i) / (LEGEND_STEPS - 1) - 1;
    ctx.fillStyle = treeExplorerDiffColor(score).cssString;
    ctx.fillRect(i, 0, 1, 1);
  }
}
