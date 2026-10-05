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

import './flamegraph_diff.scss';
import m from 'mithril';
import {assertUnreachable} from '../base/assert';
import {classNames} from '../base/classnames';
import type {ExportFormat} from './export_button';
import {
  formatAsJSON,
  formatAsMarkdown,
  formatAsTSV,
} from '../base/export_formatters';
import {
  type TreeExplorerData,
  type TreeExplorerMetric,
  type TreeExplorerNode,
  type TreeExplorerOptionalAction,
  displaySize,
  getUnitDisplayName,
} from './tree_explorer';
import {
  displayChange,
  displayChangePercentage,
  displaySignedSize,
  isPairingProperty,
} from './tree_explorer_diff';

export function flamegraphDiffRootLabel(
  baselineCumulative: number,
  currentCumulative: number,
  unit: string,
): string {
  return `root: ${displayChange(baselineCumulative, currentCumulative, unit)}`;
}

// The values of a node (or of the root) in the baseline and current trees of
// a diff, and how they changed.
function renderDiffTable(
  unit: string,
  rows: ReadonlyArray<{
    readonly label: string;
    readonly baseline: number;
    readonly current: number;
  }>,
): m.Children {
  return m(
    'table.pf-flamegraph-diff-table',
    m(
      'thead',
      m(
        'tr',
        m('th'),
        m('th', 'Baseline'),
        m('th', 'Current'),
        m('th', 'Change'),
      ),
    ),
    m(
      'tbody',
      rows.map(({label, baseline, current}) => {
        const delta = current - baseline;
        return m(
          'tr',
          m('th', label),
          m('td', displaySize(baseline, unit)),
          m('td', displaySize(current, unit)),
          m(
            'td',
            {
              className: classNames(
                delta > 0 && 'pf-flamegraph-diff-table__change--grew',
                delta < 0 && 'pf-flamegraph-diff-table__change--shrank',
              ),
            },
            `${displaySignedSize(delta, unit)} (${displayChangePercentage(
              baseline,
              current,
            )})`,
          ),
        );
      }),
    ),
  );
}

export function renderFlamegraphDiffRootTooltip(
  unit: string,
  baselineAllRootsCumulativeValue: number,
  allRootsCumulativeValue: number,
  actionsMenu: m.Children,
): m.Children {
  return m(
    'div',
    m('.tooltip-text-line', m('.tooltip-bold-text', 'root'), actionsMenu),
    renderDiffTable(unit, [
      {
        label: 'Cumulative',
        baseline: baselineAllRootsCumulativeValue,
        current: allRootsCumulativeValue,
      },
    ]),
  );
}

export function renderFlamegraphDiffNodeTooltip(
  unit: string,
  node: TreeExplorerNode,
): m.Children {
  return renderDiffTable(unit, [
    {
      label: 'Cumulative',
      baseline: node.baselineCumulativeValue ?? 0,
      current: node.cumulativeValue,
    },
    {
      label: 'Self',
      baseline: node.baselineSelfValue ?? 0,
      current: node.selfValue,
    },
  ]);
}

// The host's actions act on the current tree of a diff, which the nodes
// only the baseline tree has are missing from.
export function filterDiffHostActions(
  nodeActions: ReadonlyArray<TreeExplorerOptionalAction>,
  isDiff: boolean,
  cumulativeValue: number,
): ReadonlyArray<TreeExplorerOptionalAction> {
  if (isDiff && cumulativeValue === 0) {
    return [];
  }
  return nodeActions;
}

export function formatDiffStackColumns(
  node: TreeExplorerNode,
  unit: string,
): [string, string] {
  return [
    displayChange(
      node.baselineCumulativeValue ?? 0,
      node.cumulativeValue,
      unit,
    ),
    displayChange(node.baselineSelfValue ?? 0, node.selfValue, unit),
  ];
}

// A diff exports each value in the baseline and current trees and its
// change, and only the pairing properties, i.e. those of both trees.
export function buildFlamegraphDiffExportString(
  data: TreeExplorerData,
  metric: TreeExplorerMetric,
  format: ExportFormat,
): string {
  const {nodes} = data;
  const unitDisplay = getUnitDisplayName(metric.unit);

  const unaggKeys: string[] = [];
  const aggKeys: string[] = [];
  const propDisplayNames = new Map<string, string>();
  for (const node of nodes) {
    for (const [key, prop] of node.properties) {
      if (!isPairingProperty(prop)) {
        continue;
      }
      const keys = prop.isAggregatable ? aggKeys : unaggKeys;
      if (!keys.includes(key)) {
        keys.push(key);
        propDisplayNames.set(key, prop.displayName);
      }
    }
  }

  const columns = [
    'id',
    'parentId',
    'depth',
    'name',
    ...unaggKeys,
    'baselineCumulativeValue',
    'cumulativeValue',
    'cumulativeDelta',
    'baselineSelfValue',
    'selfValue',
    'selfDelta',
    ...aggKeys,
  ];
  const columnNames: Record<string, string> = {
    ...Object.fromEntries(propDisplayNames),
    id: 'Id',
    parentId: 'Parent Id',
    depth: 'Depth',
    name: metric.nameColumnLabel ?? 'Name',
    cumulativeValue: `Current Cumulative ${metric.name} (${unitDisplay})`,
    selfValue: `Current Self ${metric.name} (${unitDisplay})`,
    baselineCumulativeValue: `Baseline Cumulative ${metric.name} (${unitDisplay})`,
    cumulativeDelta: `Change Cumulative ${metric.name} (${unitDisplay})`,
    baselineSelfValue: `Baseline Self ${metric.name} (${unitDisplay})`,
    selfDelta: `Change Self ${metric.name} (${unitDisplay})`,
  };
  const rows = nodes.map((n) => {
    const baselineCumulative = n.baselineCumulativeValue ?? 0;
    const baselineSelf = n.baselineSelfValue ?? 0;
    const row: Record<string, string> = {
      id: n.id.toString(),
      parentId: n.parentId.toString(),
      depth: n.depth.toString(),
      name: n.name,
      baselineCumulativeValue: baselineCumulative.toString(),
      cumulativeValue: n.cumulativeValue.toString(),
      cumulativeDelta: (n.cumulativeValue - baselineCumulative).toString(),
      baselineSelfValue: baselineSelf.toString(),
      selfValue: n.selfValue.toString(),
      selfDelta: (n.selfValue - baselineSelf).toString(),
    };
    for (const key of [...unaggKeys, ...aggKeys]) {
      row[key] = n.properties.get(key)?.value ?? '';
    }
    return row;
  });

  switch (format) {
    case 'tsv':
      return formatAsTSV(columns, columnNames, rows);
    case 'json':
      return formatAsJSON(columns, columnNames, rows);
    case 'markdown':
      return formatAsMarkdown(columns, columnNames, rows);
    default:
      assertUnreachable(format);
  }
}
