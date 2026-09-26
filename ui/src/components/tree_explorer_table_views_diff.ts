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

import './tree_explorer_table_views_diff.scss';
import m from 'mithril';
import {assertUnreachable} from '../base/assert';
import {classNames} from '../base/classnames';
import {
  formatAsJSON,
  formatAsMarkdown,
  formatAsTSV,
} from '../base/export_formatters';
import type {ExportFormat} from '../widgets/export_button';
import {
  displaySize,
  getUnitDisplayName,
  type TreeExplorerMetric,
} from '../widgets/tree_explorer';
import {
  changePercentage,
  displaySignedSize,
} from '../widgets/tree_explorer_diff';
import type {ColumnDef, ColumnSchema} from './widgets/datagrid/datagrid_schema';
import type {Column} from './widgets/datagrid/model';
import type {SqlValue} from '../trace_processor/query_result';

// A row's values in the baseline and current trees of a diff.
interface DiffValues {
  readonly baselineTotal: number;
  readonly total: number;
  readonly baselineSelf: number;
  readonly self: number;
}

interface DiffColumns {
  readonly baselineTotal: number;
  readonly totalDelta: number;
  readonly totalDeltaPercent: number;
  readonly baselineSelf: number;
  readonly selfDelta: number;
  readonly selfDeltaPercent: number;
}

// The columns (see diffColumnSchema) which diffs add to a row: the baseline
// values and how the values changed.
export function diffColumns({
  baselineTotal,
  total,
  baselineSelf,
  self,
}: DiffValues): DiffColumns {
  return {
    baselineTotal,
    totalDelta: total - baselineTotal,
    totalDeltaPercent: relativeChange(baselineTotal, total),
    baselineSelf,
    selfDelta: self - baselineSelf,
    selfDeltaPercent: relativeChange(baselineSelf, self),
  };
}

// The change from `baseline` to `current` in percent of `baseline`. New
// values have no baseline to relate to: they are +Infinity, which sorts them
// as the largest relative growth.
function relativeChange(baseline: number, current: number): number {
  return changePercentage(baseline, current) ?? Infinity;
}

export function diffColumnSchema(unit: string): ColumnSchema {
  const value = (title: string): ColumnDef => ({
    title,
    columnType: 'quantitative',
    cellRenderer: (v) => (typeof v === 'number' ? displaySize(v, unit) : ''),
  });
  const delta = (title: string): ColumnDef => ({
    title,
    columnType: 'quantitative',
    cellRenderer: (v) => renderDelta(v, (d) => displaySignedSize(d, unit)),
  });
  const deltaPercent = (title: string): ColumnDef => ({
    title,
    columnType: 'quantitative',
    cellRenderer: (v) =>
      renderDelta(v, (d) =>
        d === Infinity ? 'new' : `${d > 0 ? '+' : ''}${d.toFixed(1)}%`,
      ),
  });
  return {
    baselineTotal: value('Baseline total'),
    totalDelta: delta('Total Δ'),
    totalDeltaPercent: deltaPercent('Total Δ %'),
    baselineSelf: value('Baseline self'),
    selfDelta: delta('Self Δ'),
    selfDeltaPercent: deltaPercent('Self Δ %'),
  };
}

// A change, colored by whether the value grew or shrank.
function renderDelta(
  value: SqlValue,
  format: (delta: number) => string,
): m.Children {
  if (typeof value !== 'number') {
    return '';
  }
  return m(
    'span',
    {
      className: classNames(
        value > 0 && 'pf-tree-explorer__delta--grew',
        value < 0 && 'pf-tree-explorer__delta--shrank',
      ),
    },
    format(value),
  );
}

export const DIFF_TREE_INITIAL_COLUMNS: readonly Column[] = [
  {id: 'name', field: 'name'},
  {id: 'baselineTotal', field: 'baselineTotal'},
  {id: 'total', field: 'total'},
  {id: 'totalDelta', field: 'totalDelta', sort: 'DESC'},
  {id: 'totalDeltaPercent', field: 'totalDeltaPercent'},
  {id: 'selfDelta', field: 'selfDelta'},
];

export const DIFF_FLAT_INITIAL_COLUMNS: readonly Column[] = [
  {id: 'name', field: 'name'},
  {id: 'baselineSelf', field: 'baselineSelf'},
  {id: 'self', field: 'self'},
  {id: 'selfDelta', field: 'selfDelta', sort: 'DESC'},
  {id: 'selfDeltaPercent', field: 'selfDeltaPercent'},
  {id: 'totalDelta', field: 'totalDelta'},
];

// A function's values in the baseline and current trees of a diff (see
// computeFlatFunctions).
export interface DiffFlatFunction {
  readonly name: string;
  readonly self: number;
  readonly total: number;
  readonly baselineSelf?: number;
  readonly baselineTotal?: number;
}

// Exports both trees' values of each function and their changes, biggest
// self growth first.
export function buildFlatDiffExportString(
  flatFunctions: ReadonlyArray<DiffFlatFunction>,
  metric: TreeExplorerMetric,
  format: ExportFormat,
): string {
  const unitDisplay = getUnitDisplayName(metric.unit);
  const columns = [
    'name',
    'baselineSelf',
    'self',
    'selfDelta',
    'selfDeltaPercent',
    'baselineTotal',
    'total',
    'totalDelta',
    'totalDeltaPercent',
  ];
  const columnNames: Record<string, string> = {
    name: metric.nameColumnLabel ?? 'Name',
    baselineSelf: `Baseline Self ${metric.name} (${unitDisplay})`,
    self: `Current Self ${metric.name} (${unitDisplay})`,
    selfDelta: `Change Self ${metric.name} (${unitDisplay})`,
    selfDeltaPercent: 'Change Self %',
    baselineTotal: `Baseline Total ${metric.name} (${unitDisplay})`,
    total: `Current Total ${metric.name} (${unitDisplay})`,
    totalDelta: `Change Total ${metric.name} (${unitDisplay})`,
    totalDeltaPercent: 'Change Total %',
  };
  const percentColumns = new Set(['selfDeltaPercent', 'totalDeltaPercent']);
  const rows = flatFunctions
    .map((fn) => ({
      name: fn.name,
      self: fn.self,
      total: fn.total,
      ...diffColumns({
        baselineTotal: fn.baselineTotal ?? 0,
        total: fn.total,
        baselineSelf: fn.baselineSelf ?? 0,
        self: fn.self,
      }),
    }))
    .sort((a, b) => b.selfDelta - a.selfDelta)
    .map((row) => {
      const values: Record<string, string> = {};
      for (const column of columns) {
        const value = row[column as keyof typeof row];
        if (typeof value !== 'number') {
          values[column] = String(value ?? '');
        } else if (!Number.isFinite(value)) {
          // A relative change without a baseline value to relate to.
          values[column] = '';
        } else {
          values[column] = percentColumns.has(column)
            ? value.toFixed(2)
            : value.toString();
        }
      }
      return values;
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
