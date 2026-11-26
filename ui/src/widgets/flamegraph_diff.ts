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

// Colors, legend and tooltip table of a tree compared with a baseline (see
// TreeExplorerData.diff).

import './flamegraph_diff.scss';
import m from 'mithril';
import {classNames} from '../base/classnames';
import {HSLColor, hexToRgb, rgbToHsl} from '../base/color';
import {
  type TreeExplorerDiff,
  displayChange,
  displayRelativeChange,
  displaySize,
} from './tree_explorer';

// The colors of a change from the largest shrink (-1) through no change (0)
// to the largest growth (1): green, grey, yellow and red.
const DIFF_COLOR_STOPS: ReadonlyArray<readonly [number, string]> = [
  [-1, '#86d68c'],
  [0, '#dddddd'],
  [0.25, '#eed95e'],
  [1, '#d95b54'],
];

// A change scaled into [-1, 1] by the largest change in its direction.
export function diffScore(change: number, diff: TreeExplorerDiff): number {
  if (change > 0) {
    return diff.maxGrowth > 0 ? Math.min(change / diff.maxGrowth, 1) : 1;
  }
  if (change < 0) {
    return diff.maxShrink > 0 ? -Math.min(-change / diff.maxShrink, 1) : -1;
  }
  return 0;
}

export function diffColor(score: number): HSLColor {
  let i = 1;
  while (i < DIFF_COLOR_STOPS.length - 1 && score > DIFF_COLOR_STOPS[i]![0]) {
    i++;
  }
  const [from, fromHex] = DIFF_COLOR_STOPS[i - 1]!;
  const [to, toHex] = DIFF_COLOR_STOPS[i]!;
  const t = (score - from) / (to - from);
  const a = hexToRgb(fromHex);
  const b = hexToRgb(toHex);
  const mix = (k: 0 | 1 | 2) => Math.round(a[k] + (b[k] - a[k]) * t);
  return new HSLColor(rgbToHsl([mix(0), mix(1), mix(2)]));
}

// Formats a value of a diff, e.g. "23.23 MiB → 26.39 MiB (+3.16 MiB, +13.60%)".
export function displayDiff(
  baseline: number,
  current: number,
  unit: string,
): string {
  const change = displayChange(current - baseline, unit);
  const relative = displayRelativeChange(baseline, current);
  return (
    `${displaySize(baseline, unit)} → ${displaySize(current, unit)} ` +
    `(${change}, ${relative})`
  );
}

// The colors of the changes, from the largest shrink to the largest growth.
export function renderDiffLegend(
  diff: TreeExplorerDiff,
  unit: string,
): m.Children {
  const paint = ({dom}: m.VnodeDOM) =>
    paintLegend(dom as HTMLCanvasElement, diff);
  return m(
    '.pf-flamegraph-diff-legend',
    displayChange(-diff.maxShrink, unit),
    m('canvas.pf-flamegraph-diff-legend__scale', {
      width: LEGEND_STEPS,
      height: 1,
      oncreate: paint,
      onupdate: paint,
    }),
    displayChange(diff.maxGrowth, unit),
  );
}

const LEGEND_STEPS = 64;

function paintLegend(canvas: HTMLCanvasElement, diff: TreeExplorerDiff) {
  const ctx = canvas.getContext('2d');
  if (ctx === null) return;
  const range = diff.maxShrink + diff.maxGrowth;
  for (let i = 0; i < LEGEND_STEPS; i++) {
    const change = -diff.maxShrink + ((i + 0.5) / LEGEND_STEPS) * range;
    ctx.fillStyle = diffColor(diffScore(change, diff)).cssString;
    ctx.fillRect(i, 0, 1, 1);
  }
}

export interface DiffTableRow {
  readonly label: string;
  readonly baseline: number;
  readonly current: number;
  readonly unit: string;
  // Visible totals of all values on each side; when set, adds an "all:"
  // sub-line showing each side's share and their difference in percentage points.
  readonly allTotals?: {
    readonly baseline: number;
    readonly current: number;
  };
}

// Formats a difference of percentages in percentage points, e.g. "+11.3 pp".
export function displayPercentPointDelta(delta: number, decimals = 1): string {
  // Avoid "-0.0 pp" for tiny negative deltas that round to zero.
  const rounded = Number(delta.toFixed(decimals));
  if (rounded === 0) return `${(0).toFixed(decimals)} pp`;
  return `${rounded > 0 ? '+' : ''}${rounded.toFixed(decimals)} pp`;
}

// Sign of a value after rounding to `decimals`, so a value displayed as zero
// is not colored as growing or shrinking.
export function roundedSign(value: number, decimals = 1): -1 | 0 | 1 {
  const rounded = Number(value.toFixed(decimals));
  if (rounded > 0) return 1;
  if (rounded < 0) return -1;
  return 0;
}

// The values of a node (or of the root) in the current and baseline trees,
// and how they changed.
export function renderDiffTable(rows: ReadonlyArray<DiffTableRow>): m.Children {
  return m(
    'table.pf-flamegraph-diff-table',
    m(
      'thead',
      m(
        'tr',
        m('th'),
        m('th', 'Current'),
        m('th', 'Baseline'),
        m('th', 'Current − Baseline'),
      ),
    ),
    m(
      'tbody',
      rows.map(({label, baseline, current, unit, allTotals}) => {
        const delta = current - baseline;
        const changeClass = classNames(
          delta > 0 && 'pf-flamegraph-diff-table__change--grew',
          delta < 0 && 'pf-flamegraph-diff-table__change--shrank',
        );
        const styled = (text: string) =>
          changeClass ? m('span', {className: changeClass}, text) : text;
        const currentShare =
          allTotals === undefined
            ? undefined
            : allTotals.current === 0
              ? 0
              : (current / allTotals.current) * 100;
        const baselineShare =
          allTotals === undefined
            ? undefined
            : allTotals.baseline === 0
              ? 0
              : (baseline / allTotals.baseline) * 100;
        const ppDelta =
          currentShare !== undefined && baselineShare !== undefined
            ? currentShare - baselineShare
            : undefined;
        const ppSign = ppDelta === undefined ? 0 : roundedSign(ppDelta, 2);
        return m(
          'tr',
          m(
            'th',
            m('div', label),
            allTotals !== undefined &&
              m('.pf-flamegraph-diff-table__sub', '% of all'),
          ),
          m(
            'td',
            m('div', displaySize(current, unit)),
            currentShare !== undefined &&
              m(
                '.pf-flamegraph-diff-table__sub',
                `${currentShare.toFixed(2)}%`,
              ),
          ),
          m(
            'td',
            m('div', displaySize(baseline, unit)),
            baselineShare !== undefined &&
              m(
                '.pf-flamegraph-diff-table__sub',
                `${baselineShare.toFixed(2)}%`,
              ),
          ),
          m(
            'td',
            m(
              'div',
              styled(displayChange(delta, unit)),
              ' (',
              styled(displayRelativeChange(baseline, current)),
              ')',
            ),
            ppDelta !== undefined &&
              m(
                '.pf-flamegraph-diff-table__sub',
                {
                  className: classNames(
                    ppSign > 0 && 'pf-flamegraph-diff-table__sub--grew',
                    ppSign < 0 && 'pf-flamegraph-diff-table__sub--shrank',
                  ),
                },
                displayPercentPointDelta(ppDelta, 2),
              ),
          ),
        );
      }),
    ),
  );
}
