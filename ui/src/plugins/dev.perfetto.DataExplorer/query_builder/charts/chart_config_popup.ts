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
import {ChartConfig, BarOrientation} from '../nodes/visualisation_node';
import {
  CHART_TYPES,
  getChartTypeDefinition,
  isValidChartType,
} from '../nodes/chart_type_registry';
import {ChartAggregation} from '../../../../components/widgets/charts/chart_utils';
import {Select} from '../../../../widgets/select';
import {Form, FormLabel} from '../../../../widgets/form';
import {Checkbox} from '../../../../widgets/checkbox';
import {ChartColumnProvider} from './chart_renderers';

interface ColumnInfo {
  readonly name: string;
}

/**
 * Pick the right measure column when the aggregation type changes.
 * Returns undefined when no change is needed, or the new column name
 * (or undefined to clear it).
 */
function pickMeasureColumn(
  agg: ChartAggregation,
  currentCol: string | undefined,
  numericColumns: readonly ColumnInfo[],
  allColumns: readonly ColumnInfo[],
): string | undefined {
  if (agg === 'COUNT') {
    // COUNT doesn't need a measure column.
    return currentCol;
  }
  if (agg === 'COUNT_DISTINCT') {
    // COUNT_DISTINCT works on any column; keep current or pick first.
    return (
      currentCol ?? (allColumns.length > 0 ? allColumns[0].name : undefined)
    );
  }
  // Numeric aggregations: reset if current column isn't numeric.
  const numericNames = new Set(numericColumns.map((c) => c.name));
  if (currentCol && numericNames.has(currentCol)) {
    return currentCol;
  }
  return numericColumns.length > 0 ? numericColumns[0].name : undefined;
}

export interface ChartConfigPopupContext {
  readonly node: ChartColumnProvider;
  readonly onFilterChange?: () => void;
}

export interface ChartConfigPopupOptions {
  /** When true, the chart type <select> row is omitted. */
  readonly hideChartTypeSelector?: boolean;
}

/**
 * Render the settings popup for a single chart.
 *
 * @param ctx - Node and filter-change callback.
 * @param config - The chart configuration being edited.
 * @param onBinCountDebounce - Called after bin count input so the caller can
 *   schedule a debounced redraw without exposing timer state here.
 */
export function renderChartConfigPopup(
  ctx: ChartConfigPopupContext,
  config: ChartConfig,
  onBinCountDebounce: () => void,
  options?: ChartConfigPopupOptions,
): m.Child {
  const def = getChartTypeDefinition(config.chartType);
  const chartableColumns = ctx.node.getChartableColumns(config.chartType);
  const allColumns = ctx.node.sourceCols;
  const numericColumns = ctx.node.getChartableColumns('histogram');
  const hasNumericColumns = numericColumns.length > 0;

  // Show measure column when aggregation requires a column (not COUNT).
  const showMeasureColumn =
    def?.supportsAggregation &&
    config.aggregation !== undefined &&
    config.aggregation !== 'COUNT';

  const measureColumnLabel = 'Measure Column';

  // Orientation only applies to bar charts.
  const showOrientation = config.chartType === 'bar';

  const showDelete = ctx.node.state.chartConfigs.length > 1;

  return m(
    Form,
    {
      submitLabel: showDelete ? 'Delete Chart' : undefined,
      submitIcon: showDelete ? 'delete' : undefined,
      onSubmit: () => {
        ctx.node.removeChart(config.id);
        ctx.onFilterChange?.();
      },
    },
    [
      // Chart type selector
      !options?.hideChartTypeSelector &&
        m(FormLabel, [
          m('span', 'Chart Type'),
          m(
            Select,
            {
              value: config.chartType,
              onchange: (e: Event) => {
                const target = e.target as HTMLSelectElement;
                const newType = target.value;
                if (!isValidChartType(newType)) return;
                const newChartableColumns =
                  ctx.node.getChartableColumns(newType);
                const columnStillValid = newChartableColumns.some(
                  (c) => c.name === config.column,
                );
                ctx.node.updateChart(config.id, {
                  chartType: newType,
                  column: columnStillValid ? config.column : '',
                });
              },
            },
            CHART_TYPES.map((chartTypeDef) =>
              m(
                'option',
                {
                  value: chartTypeDef.type,
                  disabled:
                    chartTypeDef.requiresNumericDimension &&
                    ctx.node.getChartableColumns(chartTypeDef.type).length ===
                      0,
                },
                chartTypeDef.label,
              ),
            ),
          ),
        ]),
      // Primary column selector
      m(FormLabel, [
        m('span', def?.primaryColumnLabel ?? 'Column'),
        m(
          Select,
          {
            value: config.column,
            onchange: (e: Event) => {
              const target = e.target as HTMLSelectElement;
              ctx.node.updateChart(config.id, {column: target.value});
            },
          },
          [
            m('option', {value: '', disabled: true}, 'Select column...'),
            ...chartableColumns.map((col) =>
              m('option', {value: col.name}, col.name),
            ),
          ],
        ),
      ]),
      // Y column — for line, scatter, boxplot, heatmap
      def?.supportsYColumn &&
        m(FormLabel, [
          m('span', def.yColumnLabel ?? 'Y Column'),
          m(
            Select,
            {
              value: config.yColumn ?? '',
              onchange: (e: Event) => {
                const target = e.target as HTMLSelectElement;
                ctx.node.updateChart(config.id, {yColumn: target.value});
              },
            },
            [
              m('option', {value: '', disabled: true}, 'Select column...'),
              ...numericColumns.map((col) =>
                m('option', {value: col.name}, col.name),
              ),
            ],
          ),
        ]),
      // Aggregation — bar, pie, treemap
      def?.supportsAggregation &&
        m(FormLabel, [
          m('span', 'Aggregation'),
          m(
            Select,
            {
              value: config.aggregation ?? 'COUNT',
              onchange: (e: Event) => {
                const target = e.target as HTMLSelectElement;
                const agg = target.value as ChartAggregation;
                const updates: Partial<ChartConfig> = {aggregation: agg};
                const picked = pickMeasureColumn(
                  agg,
                  config.measureColumn,
                  numericColumns,
                  allColumns,
                );
                if (picked !== config.measureColumn) {
                  updates.measureColumn = picked;
                }
                ctx.node.updateChart(config.id, updates);
              },
            },
            [
              m('option', {value: 'COUNT'}, 'Count'),
              m('option', {value: 'COUNT_DISTINCT'}, 'Count Distinct'),
              m('option', {value: 'SUM', disabled: !hasNumericColumns}, 'Sum'),
              m('option', {value: 'AVG', disabled: !hasNumericColumns}, 'Avg'),
              m('option', {value: 'MIN', disabled: !hasNumericColumns}, 'Min'),
              m('option', {value: 'MAX', disabled: !hasNumericColumns}, 'Max'),
              m('option', {value: 'P25', disabled: !hasNumericColumns}, 'P25'),
              m('option', {value: 'P50', disabled: !hasNumericColumns}, 'P50'),
              m('option', {value: 'P75', disabled: !hasNumericColumns}, 'P75'),
              m('option', {value: 'P90', disabled: !hasNumericColumns}, 'P90'),
              m('option', {value: 'P95', disabled: !hasNumericColumns}, 'P95'),
              m('option', {value: 'P99', disabled: !hasNumericColumns}, 'P99'),
            ],
          ),
        ]),
      // Measure / size column
      showMeasureColumn &&
        m(FormLabel, [
          m('span', measureColumnLabel),
          m(
            Select,
            {
              value: config.measureColumn ?? '',
              onchange: (e: Event) => {
                const target = e.target as HTMLSelectElement;
                ctx.node.updateChart(config.id, {
                  measureColumn: target.value,
                });
              },
            },
            [
              m(
                'option',
                {value: '', disabled: true},
                `Select ${measureColumnLabel.toLowerCase()}...`,
              ),
              // COUNT_DISTINCT works on any column type; others need numeric.
              ...(config.aggregation === 'COUNT_DISTINCT'
                ? allColumns
                : numericColumns
              ).map((col) => m('option', {value: col.name}, col.name)),
            ],
          ),
        ]),
      // Group / series column
      def?.supportsGroupColumn &&
        m(FormLabel, [
          m('span', 'Series Column'),
          m(
            Select,
            {
              value: config.groupColumn ?? '',
              onchange: (e: Event) => {
                const target = e.target as HTMLSelectElement;
                ctx.node.updateChart(config.id, {
                  groupColumn: target.value || undefined,
                });
              },
            },
            [
              m('option', {value: ''}, '(none)'),
              ...allColumns.map((col) =>
                m('option', {value: col.name}, col.name),
              ),
            ],
          ),
        ]),
      // Bubble size column — scatter only
      def?.supportsSizeColumn &&
        m(FormLabel, [
          m('span', 'Size Column'),
          m(
            Select,
            {
              value: config.sizeColumn ?? '',
              onchange: (e: Event) => {
                const target = e.target as HTMLSelectElement;
                ctx.node.updateChart(config.id, {
                  sizeColumn: target.value || undefined,
                });
              },
            },
            [
              m('option', {value: ''}, '(none)'),
              ...numericColumns.map((col) =>
                m('option', {value: col.name}, col.name),
              ),
            ],
          ),
        ]),
      // Orientation — bar only
      showOrientation &&
        m(FormLabel, [
          m('span', 'Orientation'),
          m(
            Select,
            {
              value: config.orientation ?? 'vertical',
              onchange: (e: Event) => {
                const target = e.target as HTMLSelectElement;
                const orientation = target.value as BarOrientation;
                ctx.node.updateChart(config.id, {orientation});
              },
            },
            [
              m('option', {value: 'vertical'}, 'Vertical'),
              m('option', {value: 'horizontal'}, 'Horizontal'),
            ],
          ),
        ]),
      // Bin count — histogram only
      def?.supportsBinning &&
        m(FormLabel, [
          m('span', 'Bin Count'),
          m('input', {
            type: 'number',
            min: 1,
            max: 100,
            value: config.binCount ?? '',
            placeholder: 'Auto',
            oninput: (e: Event) => {
              const target = e.target as HTMLInputElement;
              const value = target.value.trim();

              let binCount: number | undefined;
              if (value === '') {
                binCount = undefined;
              } else {
                const parsed = parseInt(value, 10);
                binCount =
                  !Number.isNaN(parsed) && parsed > 0 ? parsed : undefined;
              }
              ctx.node.updateChart(config.id, {binCount});
              onBinCountDebounce();
            },
          }),
        ]),
      m(FormLabel, [
        m(Checkbox, {
          label: 'Show truncation warning',
          checked: config.showTruncationWarning ?? true,
          onchange: (e: Event) => {
            const target = e.target as HTMLInputElement;
            ctx.node.updateChart(config.id, {
              showTruncationWarning: target.checked,
            });
          },
        }),
      ]),
    ],
  );
}
