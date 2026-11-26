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
import {z} from 'zod';
import {moveItem} from '../../../base/array_utils';
import {AsyncMemo, TASK_CANCELLED} from '../../../base/async_memo';
import {Duration} from '../../../base/time';
import type {NodeManifest, DetailsContext} from '../node_types';
import {Button, ButtonVariant} from '../../../widgets/button';
import {EmptyState} from '../../../widgets/empty_state';
import {Spinner} from '../../../widgets/spinner';
import {NUM_NULL, STR_NULL} from '../../../trace_processor/query_result';
import {ColumnPicker} from '../widgets/column_picker';
import {DraggableStack} from '../components/draggable_stack';
import {InputPortStepper} from '../components/input_port_stepper';
import {Stack} from '../components/stack';
import './chart.scss';
import type {Port} from '../graph_model';
import {manifest as unionManifest} from './union';

const ChartSpecSchema = z.object({
  xCol: z.string(),
  yCol: z.string().describe('Measure column to SUM, or "" for COUNT(*)'),
});

export type ChartSpec = z.infer<typeof ChartSpecSchema>;

const ChartConfigSchema = z
  .object({
    charts: z.array(ChartSpecSchema),
    numInputs: z.number(),
  })
  .describe(
    'Render bar charts in the details panel while passing input rows through.',
  );

export type ChartConfig = z.infer<typeof ChartConfigSchema>;

// ---------------------------------------------------------------------------
// Bar chart component — renders one horizontal bar chart for a single spec.
// ---------------------------------------------------------------------------

interface BarRow {
  readonly label: string;
  readonly value: number;
}

type BarChartQueryResult =
  | {readonly ok: true; readonly rows: readonly BarRow[]}
  | {readonly ok: false; readonly error: string};

const CHART_LIMIT = 20;
const CHART_COLOR_COUNT = 8;

interface ChartSource {
  readonly sql: string;
  readonly includes: readonly string[];
}

function formatChartValue(value: number, isDuration: boolean): string {
  if (isDuration && Number.isFinite(value)) {
    return Duration.humanise(Duration.fromRaw(BigInt(Math.round(value))));
  }
  if (Number.isInteger(value)) {
    return value.toLocaleString();
  }
  return value.toPrecision(4);
}

function BarChartPanel(): m.Component<{
  readonly spec: ChartSpec;
  readonly source: ChartSource;
  readonly ctx: DetailsContext;
}> {
  const queryMemo = new AsyncMemo<BarChartQueryResult>();

  return {
    onremove() {
      queryMemo.dispose();
    },
    view({attrs}) {
      const {spec, source, ctx} = attrs;
      const title = spec.xCol
        ? spec.yCol
          ? `${spec.xCol} × ${spec.yCol}`
          : `${spec.xCol} (count)`
        : '(no column selected)';

      if (!spec.xCol) {
        return m('.pf-spag-chart-panel', [
          m('.pf-spag-chart-title', title),
          m('.pf-spag-chart-empty', 'Pick an x column in the Chart node.'),
        ]);
      }

      const result = queryMemo.use({
        key: {
          sql: source.sql,
          includes: [...source.includes],
          xCol: spec.xCol,
          yCol: spec.yCol,
        },
        compute: async (
          signal,
        ): Promise<BarChartQueryResult | typeof TASK_CANCELLED> => {
          const valueExpr = spec.yCol
            ? `SUM(CAST(${spec.yCol} AS REAL))`
            : 'COUNT(*)';
          const sql =
            source.includes
              .map((inc) => `INCLUDE PERFETTO MODULE ${inc};\n`)
              .join('') +
            `SELECT CAST(${spec.xCol} AS TEXT) AS label, ${valueExpr} AS value ` +
            `FROM (${source.sql}) ` +
            `GROUP BY ${spec.xCol} ` +
            `ORDER BY value DESC ` +
            `LIMIT ${CHART_LIMIT}`;

          try {
            const queryRes = await ctx.trace.engine.query(sql);
            if (signal.isCancelled) return TASK_CANCELLED;
            const rows: BarRow[] = [];
            const it = queryRes.iter({label: STR_NULL, value: NUM_NULL});
            for (; it.valid(); it.next()) {
              rows.push({
                label: it.label ?? '(null)',
                value: Number(it.value ?? 0),
              });
            }
            return {ok: true, rows};
          } catch (e: unknown) {
            if (signal.isCancelled) return TASK_CANCELLED;
            return {
              ok: false,
              error: e instanceof Error ? e.message : String(e),
            };
          }
        },
      });

      if (result.data === undefined) {
        return m('.pf-spag-chart-panel', [
          m('.pf-spag-chart-title', title),
          m('.pf-spag-chart-loading', [m(Spinner), ' Loading…']),
        ]);
      }

      if (!result.data.ok) {
        return m('.pf-spag-chart-panel', [
          m('.pf-spag-chart-title', title),
          m('.pf-spag-chart-error', result.data.error),
        ]);
      }

      const rows = result.data.rows;
      if (rows.length === 0) {
        return m('.pf-spag-chart-panel', [
          m('.pf-spag-chart-title', title),
          m('.pf-spag-chart-empty', 'No data'),
        ]);
      }

      const yColDef = spec.yCol
        ? ctx.outputColumns?.find((c) => c.name === spec.yCol)
        : undefined;
      const isDuration = yColDef?.type?.kind === 'duration';
      const maxValue = Math.max(...rows.map((r) => Math.abs(r.value)), 1);

      return m('.pf-spag-chart-panel', [
        m('.pf-spag-chart-header', [
          m('.pf-spag-chart-title', title),
          m(
            'span.pf-spag-chart-meta',
            rows.length >= CHART_LIMIT
              ? `top ${CHART_LIMIT}`
              : `${rows.length} ${rows.length === 1 ? 'group' : 'groups'}`,
          ),
        ]),
        m(
          '.pf-spag-chart-bars',
          rows.map((row, i) => {
            const pct = Math.max(1, (Math.abs(row.value) / maxValue) * 100);
            const colorIdx = (i % CHART_COLOR_COUNT) + 1;
            const valueStr = formatChartValue(row.value, isDuration);
            return m(
              '.pf-spag-chart-bar-row',
              {
                key: i,
                title: `${row.label}: ${valueStr}`,
              },
              [
                m('.pf-spag-chart-bar-label', row.label),
                m(
                  '.pf-spag-chart-bar-track',
                  m('.pf-spag-chart-bar-fill', {
                    style: {
                      width: `${pct.toFixed(2)}%`,
                      background: `var(--pf-chart-color-${colorIdx})`,
                    },
                  }),
                ),
                m('.pf-spag-chart-bar-value', valueStr),
              ],
            );
          }),
        ),
      ]);
    },
  };
}

// ---------------------------------------------------------------------------
// Dashboard rendered in the details panel.
// ---------------------------------------------------------------------------

function ChartDashboard(): m.Component<{
  readonly config: ChartConfig;
  readonly ctx: DetailsContext;
}> {
  return {
    view({attrs: {config, ctx}}) {
      if (ctx.error !== undefined) {
        return m(
          EmptyState,
          {
            icon: 'warning',
            fillHeight: true,
            title: 'Query error',
          },
          m('pre.pf-spag__error', ctx.error),
        );
      }

      const source = ctx.output;
      if (source === undefined) {
        return m(
          EmptyState,
          {
            icon: 'bar_chart',
            fillHeight: true,
            title: 'No chart data',
          },
          'Connect an input to see charts.',
        );
      }

      if (config.charts.length === 0) {
        return m(
          EmptyState,
          {
            icon: 'bar_chart',
            fillHeight: true,
            title: 'No charts configured',
          },
          'Click "Add chart" on the Chart node to visualize columns.',
        );
      }

      return m(
        '.pf-spag-chart-dashboard',
        config.charts.map((spec, i) =>
          m(BarChartPanel, {
            key: i,
            spec,
            source,
            ctx,
          }),
        ),
      );
    },
  };
}

// ---------------------------------------------------------------------------
// Node manifest.
// ---------------------------------------------------------------------------

export const manifest: NodeManifest<ChartConfig> = {
  title: 'Chart',
  icon: 'bar_chart',
  getInputs(config: ChartConfig): Port[] {
    return Array.from({length: config.numInputs}, (_, i) => ({
      name: `input_${i}`,
      content: `Input ${i + 1}`,
    }));
  },
  hue: 190,
  schema: ChartConfigSchema,
  expensive: true,
  defaultConfig: () => ({charts: [], numInputs: 1}),
  render(config, updateConfig, ctx) {
    const n = config.numInputs;
    const canRemove = n > 1;
    let cols = ctx.availableColumns;
    if (cols.length === 0) {
      for (const port of ctx.inputPorts) {
        const portCols = ctx.getInputColumns(port.name);
        if (portCols.length > 0) {
          cols = portCols;
          break;
        }
      }
    }

    return m(Stack, {minWidth: 'lg'}, [
      m(InputPortStepper, {
        canRemove,
        onRemove: () => updateConfig({numInputs: n - 1}),
        onAdd: () => updateConfig({numInputs: n + 1}),
      }),
      m('.pf-spag-section-label', 'Charts'),
      m(DraggableStack, {
        deleteTitle: 'Remove chart',
        rows: config.charts.map((spec, i) => [
          m(ColumnPicker, {
            value: spec.xCol,
            columns: cols,
            placeholder: 'x column',
            onSelect: (xCol) =>
              updateConfig({charts: config.charts.with(i, {...spec, xCol})}),
          }),
          m('span.pf-spag-operator', '×'),
          m(ColumnPicker, {
            value: spec.yCol,
            columns: cols,
            placeholder: 'y (count)',
            onSelect: (yCol) =>
              updateConfig({charts: config.charts.with(i, {...spec, yCol})}),
          }),
        ]),
        onReorder: (from, to) =>
          updateConfig({charts: moveItem(config.charts, from, to)}),
        onDelete: (i) => updateConfig({charts: config.charts.toSpliced(i, 1)}),
      }),
      m(Button, {
        label: 'Add chart',
        icon: 'add',
        variant: ButtonVariant.Filled,
        onclick: () =>
          updateConfig({charts: [...config.charts, {xCol: '', yCol: ''}]}),
      }),
    ]);
  },
  getOutputColumns(_config, ctx) {
    for (const port of ctx.inputPorts) {
      const cols = ctx.getInputColumns(port.name);
      if (cols !== undefined) return cols;
    }
    return undefined;
  },
  renderDetails(config, ctx) {
    return m(ChartDashboard, {config, ctx});
  },
  // Passes rows through (UNION ALL of all inputs when there are several).
  generators: {
    sql: (config, ctx) =>
      unionManifest.generators.sql(
        {distinct: false, numInputs: config.numInputs},
        ctx,
      ),
    psn: (config, ctx) =>
      unionManifest.generators.psn?.(
        {distinct: false, numInputs: config.numInputs},
        ctx,
      ),
  },
};
