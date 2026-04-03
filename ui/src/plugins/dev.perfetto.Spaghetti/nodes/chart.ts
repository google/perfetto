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
import {ManifestPort, NodeManifest, DetailsContext} from '../node_types';
import {Button, ButtonVariant} from '../../../widgets/button';
import {NUM_NULL, STR_NULL} from '../../../trace_processor/query_result';
import {ColumnPicker} from '../widgets/column_picker';

export interface ChartSpec {
  readonly xCol: string;
  readonly yCol: string; // empty string = COUNT(*)
}

export interface ChartConfig {
  readonly charts: ChartSpec[];
}

// ---------------------------------------------------------------------------
// Bar chart component — renders one SVG bar chart for a single spec.
// ---------------------------------------------------------------------------

interface BarRow {
  label: string;
  value: number;
}

const CHART_LIMIT = 20;
const BAR_HEIGHT = 18;
const BAR_GAP = 3;
const LABEL_WIDTH = 130;
const VALUE_WIDTH = 52;
const BAR_AREA_WIDTH = 180;
const CHART_SVG_WIDTH = LABEL_WIDTH + BAR_AREA_WIDTH + VALUE_WIDTH;

function BarChartPanel(): m.Component<{
  spec: ChartSpec;
  materializedTable: string;
  ctx: DetailsContext;
}> {
  let rows: BarRow[] | undefined;
  let error: string | undefined;
  let loadingFor = '';

  function keyFor(materializedTable: string, spec: ChartSpec): string {
    return `${materializedTable}|${spec.xCol}|${spec.yCol}`;
  }

  function load(
    materializedTable: string,
    spec: ChartSpec,
    ctx: DetailsContext,
  ) {
    const k = keyFor(materializedTable, spec);
    if (loadingFor === k) return;
    loadingFor = k;
    rows = undefined;
    error = undefined;

    if (!spec.xCol) {
      rows = [];
      loadingFor = '';
      return;
    }

    const valueExpr = spec.yCol
      ? `SUM(CAST(${spec.yCol} AS REAL))`
      : 'COUNT(*)';
    const sql =
      `SELECT CAST(${spec.xCol} AS TEXT) AS label, ${valueExpr} AS value ` +
      `FROM ${materializedTable} ` +
      `GROUP BY ${spec.xCol} ` +
      `ORDER BY value DESC ` +
      `LIMIT ${CHART_LIMIT}`;

    ctx.trace.engine
      .query(sql)
      .then((result) => {
        if (loadingFor !== k) return; // stale
        const newRows: BarRow[] = [];
        const it = result.iter({label: STR_NULL, value: NUM_NULL});
        for (; it.valid(); it.next()) {
          newRows.push({
            label: it.label ?? '(null)',
            value: Number(it.value ?? 0),
          });
        }
        rows = newRows;
        m.redraw();
      })
      .catch((e: unknown) => {
        if (loadingFor !== k) return;
        error = e instanceof Error ? e.message : String(e);
        rows = [];
        m.redraw();
      });
  }

  return {
    oninit({attrs}) {
      load(attrs.materializedTable, attrs.spec, attrs.ctx);
    },
    onupdate({attrs}) {
      load(attrs.materializedTable, attrs.spec, attrs.ctx);
    },
    view({attrs}) {
      const {spec} = attrs;
      const title = spec.xCol
        ? spec.yCol
          ? `${spec.xCol} × ${spec.yCol}`
          : `${spec.xCol} (count)`
        : '(no column selected)';

      if (error) {
        return m('.pf-qb-chart-panel', [
          m('.pf-qb-chart-title', title),
          m('.pf-qb-chart-error', error),
        ]);
      }

      if (!rows) {
        return m('.pf-qb-chart-panel', [
          m('.pf-qb-chart-title', title),
          m('.pf-qb-chart-loading', 'Loading…'),
        ]);
      }

      if (rows.length === 0) {
        return m('.pf-qb-chart-panel', [
          m('.pf-qb-chart-title', title),
          m('.pf-qb-chart-empty', 'No data'),
        ]);
      }

      const maxValue = Math.max(...rows.map((r) => r.value), 1);
      const svgHeight = rows.length * (BAR_HEIGHT + BAR_GAP) + BAR_GAP;

      const bars = rows.map((row, i) => {
        const y = BAR_GAP + i * (BAR_HEIGHT + BAR_GAP);
        const barW = Math.max(2, (row.value / maxValue) * BAR_AREA_WIDTH);
        const hue = (i * 47) % 360;
        const label =
          row.label.length > 18 ? row.label.slice(0, 17) + '…' : row.label;
        const valueStr = Number.isInteger(row.value)
          ? row.value.toLocaleString()
          : row.value.toPrecision(4);

        return m('g', {key: i}, [
          m(
            'text',
            {
              'x': LABEL_WIDTH - 4,
              'y': y + BAR_HEIGHT / 2 + 4,
              'text-anchor': 'end',
              'font-size': '11',
              'fill': 'var(--pf-color-text)',
            },
            label,
          ),
          m('rect', {
            x: LABEL_WIDTH,
            y,
            width: barW,
            height: BAR_HEIGHT,
            fill: `hsl(${hue} 55% 50%)`,
            rx: 2,
          }),
          m(
            'text',
            {
              'x': LABEL_WIDTH + barW + 4,
              'y': y + BAR_HEIGHT / 2 + 4,
              'font-size': '11',
              'fill': 'var(--pf-color-text)',
            },
            valueStr,
          ),
        ]);
      });

      return m('.pf-qb-chart-panel', [
        m('.pf-qb-chart-title', title),
        m(
          'svg',
          {
            viewBox: `0 0 ${CHART_SVG_WIDTH} ${svgHeight}`,
            width: '100%',
            style: {display: 'block', overflow: 'visible'},
          },
          bars,
        ),
      ]);
    },
  };
}

// ---------------------------------------------------------------------------
// Dashboard rendered in the details panel.
// ---------------------------------------------------------------------------

function ChartDashboard(): m.Component<{
  config: ChartConfig;
  ctx: DetailsContext;
}> {
  return {
    view({attrs: {config, ctx}}) {
      if (!ctx.materializedTable) {
        return m(
          '.pf-qb-chart-dashboard',
          {
            style: {
              display: 'flex',
              alignItems: 'center',
              justifyContent: 'center',
              height: '100%',
            },
          },
          m(
            'span',
            {style: {opacity: '0.5'}},
            'Connect an input to see charts.',
          ),
        );
      }

      if (config.charts.length === 0) {
        return m(
          '.pf-qb-chart-dashboard',
          {
            style: {
              display: 'flex',
              alignItems: 'center',
              justifyContent: 'center',
              height: '100%',
            },
          },
          m(
            'span',
            {style: {opacity: '0.5'}},
            'Add chart specs in the node to see visualizations.',
          ),
        );
      }

      return m(
        '.pf-qb-chart-dashboard',
        config.charts.map((spec, i) =>
          m(BarChartPanel, {
            key: i,
            spec,
            materializedTable: ctx.materializedTable!,
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
  outputs: [{name: 'output', content: 'Output', direction: 'right'}],
  canDockTop: true,
  canDockBottom: true,
  hue: 160,
  defaultInputs(): ManifestPort[] {
    return [{name: 'input_1', content: 'Input 1', direction: 'left'}];
  },
  defaultConfig: () => ({charts: []}),
  isValid: () => true,
  getOutputColumns(_config, ctx) {
    for (const port of ctx.inputPorts) {
      const cols = ctx.getInputColumns(port.name);
      if (cols !== undefined) return cols;
    }
    return undefined;
  },
  emitIr(_config, ctx) {
    const refs = ctx.inputPorts
      .map((p) => ctx.getInputRef(p.name))
      .filter((r) => r !== '');
    if (refs.length === 0) return undefined;
    if (refs.length === 1) return {sql: `SELECT *\nFROM ${refs[0]}`};
    return {sql: refs.map((r) => `SELECT *\nFROM ${r}`).join('\nUNION ALL\n')};
  },
  render(config, updateConfig, ctx) {
    const n = ctx.inputPorts.length;
    const canRemove = ctx.removeLastInput !== undefined && n > 1;
    const cols = ctx.availableColumns;

    return m('.pf-qb-stack', [
      m('div', {style: {display: 'flex', gap: '4px'}}, [
        canRemove &&
          m(Button, {
            label: '− Input',
            variant: ButtonVariant.Filled,
            style: {flex: '1'},
            onclick: ctx.removeLastInput,
          }),
        ctx.addInput &&
          m(Button, {
            label: '+ Input',
            variant: ButtonVariant.Filled,
            style: {flex: '1'},
            onclick: () =>
              ctx.addInput!({
                name: `input_${n + 1}`,
                content: `Input ${n + 1}`,
                direction: 'left' as const,
              }),
          }),
      ]),
      m('.pf-qb-section-label', 'Charts'),
      m(
        '.pf-qb-filter-list',
        config.charts.map((spec, i) =>
          m('.pf-qb-filter-row', {key: i}, [
            m(ColumnPicker, {
              value: spec.xCol,
              columns: cols,
              placeholder: 'x column',
              onSelect: (v: string) => {
                const updated = [...config.charts];
                updated[i] = {...spec, xCol: v};
                updateConfig({charts: updated});
              },
            }),
            m(
              'span',
              {style: {opacity: '0.5', fontSize: '11px', padding: '0 2px'}},
              '×',
            ),
            m(ColumnPicker, {
              value: spec.yCol,
              columns: cols,
              placeholder: 'y (count)',
              onSelect: (v: string) => {
                const updated = [...config.charts];
                updated[i] = {...spec, yCol: v};
                updateConfig({charts: updated});
              },
            }),
            m(Button, {
              icon: 'close',
              title: 'Remove chart',
              onclick: () =>
                updateConfig({charts: config.charts.filter((_, j) => j !== i)}),
            }),
          ]),
        ),
      ),
      m(Button, {
        label: 'Add chart',
        icon: 'add',
        variant: ButtonVariant.Filled,
        onclick: () =>
          updateConfig({charts: [...config.charts, {xCol: '', yCol: ''}]}),
      }),
    ]);
  },
  renderDetails(config, ctx) {
    return m(ChartDashboard, {config, ctx});
  },
};
