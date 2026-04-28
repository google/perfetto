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

// Details view for GPU compute kernel metrics.
//
// This is the main data-fetching and rendering module for the "Details" tab.
// It builds SQL queries from the declarative Section registry,
// executes them against the trace processor, reduces the result rows into
// {@link KernelMetricData} objects, and renders the metric tables with
// optional baseline comparison and per-section analysis.
//
// Key exports:
// - {@link fetchSelectedKernelMetricData} — loads full metric data for a slice
// - {@link KernelMetricsSection} — top-level Mithril component for the tab
// - {@link renderPercentBar}, {@link renderMetricResultTable},
// {@link renderSectionList}

import m from 'mithril';
import {convertUnit, humanizeRow, humanizeSections} from './humanize';
import {Popup} from '../../widgets/popup';
import {Icon} from '../../widgets/icon';
import {Icons} from '../../base/semantic_icons';
import type {AnalysisCache} from './analysis';
import type {GpuComputeContext} from './index';
import {isTableVisible} from './section';
import {Accordion, AccordionSection} from '../../widgets/accordion';
import {Intent} from '../../widgets/common';

// =============================================================================
// SQL constants
// =============================================================================

// gpu_slice.render_stage_category value for compute dispatches.
export const COMPUTE_RENDER_STAGE_CATEGORY = 2;

// gpu_counter_group.group_id value for compute counter metrics.
export const COMPUTE_COUNTER_GROUP_ID = 6;

// =============================================================================
// Rendering utilities
// =============================================================================

// Formats a number to a fixed number of decimal places.
// Uses exponential notation for very small non-zero values.
export function formatNumber(val: number, decimals: number = 2): string {
  if (!Number.isFinite(val)) return String(val);
  const threshold = Math.pow(10, -decimals);
  if (Math.abs(val) !== 0 && Math.abs(val) < threshold) {
    return val.toExponential(decimals);
  }
  return Number.isInteger(val) ? val.toString() : val.toFixed(decimals);
}

// Renders a horizontal percent bar (0–100 %).
//
// When `baseline` is provided, draws a dual-track bar showing both the
// current value (blue) and baseline value (green) with absolute/relative
// difference labels in the overlay.
export function renderPercentBar(
  value: string | number | null,
  baseline?: number | string | null,
  showPctVal: boolean = true,
  customLabel?: string,
): m.Children {
  if (
    value == null ||
    value === 'null' ||
    value === '' ||
    value === 'undefined'
  ) {
    return m('span', 'n/a');
  }
  if (!Number.isFinite(value)) return m('span', value);

  const curPct = Number(value);
  const pctForWidth = (x: number) => Math.max(0, Math.min(100, x));

  const currentPctLabel = (() => {
    const roundedStrNum = formatNumber(curPct, 2);
    const roundedNum = Number(roundedStrNum);
    if (roundedNum === 0 && curPct !== 0) {
      return `~${curPct.toFixed(2)}%`;
    }
    return `${roundedStrNum}%`;
  })();

  if (typeof baseline === 'number' && Number.isFinite(baseline)) {
    const basePct = baseline;
    const curWidth = pctForWidth(curPct);
    const baseWidth = pctForWidth(basePct);

    const points = curPct - basePct;
    const pointsLabel = (() => {
      const sign = points >= 0 ? '+' : '';
      return sign + formatNumber(points, 2);
    })();

    const pctDiffLabel = (() => {
      if (basePct === 0) {
        if (points === 0) return '+0';
        return (points >= 0 ? '+' : '-') + 'inf';
      }
      const pct = (points / basePct) * 100;
      const sign = pct >= 0 ? '+' : '';
      return sign + formatNumber(pct, 2);
    })();

    const overlay = `${currentPctLabel} (${pointsLabel}) (${pctDiffLabel}%)`;

    return m('.pf-gpu-compute__pct-bar--dual', [
      m('.pf-gpu-compute__pct-bar-track', [
        m('.pf-gpu-compute__pct-bar-bg'),
        m('.pf-gpu-compute__pct-bar-fill', {style: `width:${curWidth}%`}),
      ]),
      m('.pf-gpu-compute__pct-bar-track', [
        m('.pf-gpu-compute__pct-bar-bg'),
        m('.pf-gpu-compute__pct-bar-fill--baseline', {
          style: `width:${baseWidth}%`,
        }),
      ]),
      m('.pf-gpu-compute__pct-bar-overlay', overlay),
    ]);
  }

  return m('.pf-gpu-compute__pct-bar', [
    m('.pf-gpu-compute__pct-bar-bg'),
    m('.pf-gpu-compute__pct-bar-fill', {
      style: `width:${pctForWidth(curPct)}%`,
    }),
    m(
      '.pf-gpu-compute__pct-bar-label',
      showPctVal ? currentPctLabel : customLabel ?? '',
    ),
  ]);
}

// =============================================================================
// Types
// =============================================================================

// A single row of metric data (label + unit + value).
export type MetricRow = {
  metric_id: string;
  metric_label: string;
  metric_unit: string;
  metric_value: number | string | null;
};

// A table within a section (description + rows).
export type MetricTable = {
  table_desc: string | null;
  data: MetricRow[];
};

// A titled group of metric tables (one per registered Section).
export type MetricSection = {
  section: string;
  tables: MetricTable[];
};

// Summary text shown in the toolbar card for the selected kernel.
export type ToolbarInfo = {
  sizeText: string;
  timeText: string;
  cyclesText: string;
  archText: string;
  smFrequencyText: string;
  processText: string;
};

// Full metric payload for a single kernel launch.
export type KernelMetricData = {
  id: number;
  kernelName: string;
  sections: MetricSection[];
  toolbar?: ToolbarInfo;
};

// Callback signature for rendering a percent-bar cell.
export type PercentBarRenderer = (
  value: number | string | null,
  baseline?: number | string | null,
) => m.Children;

// Entry in the toolbar's "Results" dropdown.
export type KernelLaunchOption = {id: number; label: string};

// =============================================================================
// Internal types — decouple from the concrete Engine types
// =============================================================================

type RowIter = {
  valid(): boolean;
  get(col: string): unknown;
  next(): void;
};

type QueryRes = {
  iter(opts: {}): RowIter;
};

type QueryCapable = {
  query(sql: string): Promise<QueryRes>;
};

// =============================================================================
// SQL query building
// =============================================================================

// Launch metrics required for display and toolbar regardless of which
// section plugins are loaded.
//
// TODO: These arg names are hardcoded to the current trace format. Consider
// adding generic protobuf fields for kernel name, grid/block dimensions, etc.
// or extending the well-known metric registry to cover launch args so plugins
// can provide alternative argument names for different vendors.
const INFRASTRUCTURE_LAUNCH_METRICS = [
  'kernel_name',
  'kernel_demangled_name',
  'launch__block_size_x',
  'launch__block_size_y',
  'launch__block_size_z',
  'launch__grid_size_x',
  'launch__grid_size_y',
  'launch__grid_size_z',
  'arch',
  'process_name',
  'process_id',
];

// Builds the shared FROM/JOIN body used by both the filtered and
// unfiltered kernel queries.  Combines infrastructure launch metrics
// with all section-declared launch metrics into EXTRACT_ARG columns.
function kernelQueryBody(ctx: GpuComputeContext): string {
  // Combine infrastructure metrics with all section-declared launch metrics.
  const allLaunchMetrics = new Set([
    ...INFRASTRUCTURE_LAUNCH_METRICS,
    ...ctx.sectionRegistry.getAllLaunchMetrics(),
  ]);

  // Build EXTRACT_ARG lines for each launch metric.
  const extractArgs = Array.from(allLaunchMetrics)
    .map((id) => `EXTRACT_ARG(s.arg_set_id, '${id}') as ${id}`)
    .join(',\n      ');

  return `
    SELECT s.id AS kernel_id, s.name AS kernel_slice_name, s.ts AS launch_ts, s.dur AS launch_dur,
           tc.name AS metric_label, SUM(c.value) AS metric_sum_value, AVG(c.value) AS metric_avg_value,
      ${extractArgs}
    FROM gpu_slice s
    INNER JOIN gpu_track tr ON tr.id = s.track_id
    LEFT JOIN counter c ON c.ts >= s.ts AND c.ts < s.ts + s.dur
      AND c.track_id IN (
        SELECT gc_tc.id FROM gpu_counter_track gc_tc
        INNER JOIN gpu_counter_group gc ON gc.track_id = gc_tc.id
          AND gc.group_id = ${COMPUTE_COUNTER_GROUP_ID}
      )
    LEFT JOIN gpu_counter_track tc ON tc.id = c.track_id
  `;
}

// Builds the full SQL query for a specific kernel slice.
export function buildKernelQuery(
  ctx: GpuComputeContext,
  sliceIdFilter: number,
): string {
  const whereFilter = `
    WHERE s.render_stage_category = ${COMPUTE_RENDER_STAGE_CATEGORY}
      AND s.id = ${sliceIdFilter}
    GROUP BY kernel_id, metric_label
  `;

  return `
    ${kernelQueryBody(ctx)}
    ${whereFilter}
    ORDER BY s.ts ASC;
  `;
}

// =============================================================================
// Data fetching
// =============================================================================

// =============================================================================
// Row reduction — SQL iterator → KernelGroup map
// =============================================================================

// Intermediate grouping of a kernel's launch args and counter metrics.
type KernelGroup = {
  kernelId: number;
  kernelName: string;
  launchTs: number;
  launchDur: number;
  metricsKV: Record<string, number | string>;
};

// Reduces the SQL result iterator into a `kernelId → KernelGroup` map.
//
// For each kernel, reads all launch-arg columns on the first row, then
// accumulates counter metrics across subsequent rows using the declared
// aggregation type (sum or avg) from the section registry.
function reduceKernelRows(
  ctx: GpuComputeContext,
  iter: RowIter,
): Map<number, KernelGroup> {
  const byId: Map<number, KernelGroup> = new Map();

  // Same combined set of launch metrics that kernelQueryBody() uses.
  const launchArgColumns = new Set([
    ...INFRASTRUCTURE_LAUNCH_METRICS,
    ...ctx.sectionRegistry.getAllLaunchMetrics(),
  ]);

  // Map from counter metric ID to declared aggregation type.
  const aggregations = ctx.sectionRegistry.getCounterAggregations();

  while (iter.valid()) {
    const kernelId = Number(iter.get('kernel_id'));

    let entry = byId.get(kernelId);
    if (!entry) {
      // First row for this kernel — read all launch-arg columns
      const metricsKV: Record<string, number | string> = {};
      for (const col of launchArgColumns) {
        const v = iter.get(col);
        metricsKV[col] = v != null ? (v as number | string) : 'n/a';
      }

      entry = {
        kernelId,
        kernelName: String(iter.get('kernel_slice_name')),
        launchTs: Number(iter.get('launch_ts')),
        launchDur: Number(iter.get('launch_dur')),
        metricsKV,
      };
      byId.set(kernelId, entry);
    }

    // Accumulate counter metrics — skip rows with no counter match
    // (LEFT JOIN produces NULL metric_label when no counters exist).
    const rawLabel = iter.get('metric_label');
    if (rawLabel != null) {
      const metricName = String(rawLabel);
      const metricSumValue = Number(iter.get('metric_sum_value'));
      const metricAvgValue = Number(iter.get('metric_avg_value'));

      // Use the declared aggregation for this counter metric.
      // Fall back to SUM for counters not declared in any section.
      const agg = aggregations.get(metricName);
      entry.metricsKV[metricName] =
        agg === 'avg' ? metricAvgValue : metricSumValue;
    }

    iter.next();
  }

  return byId;
}

// =============================================================================
// Build MetricSection[] from declarative Section definitions
// =============================================================================

// Materialises KernelMetricData from the raw `metricsKV` map
// using the declarative section registry.
//
// Steps:
// 1. Build the `availableMetrics` set (trace keys + canonical IDs).
// 2. Filter sections via `isTableVisible()`.
// 3. Map each section's table/row declarations to concrete
// {@link MetricTable} / {@link MetricRow} values.
// 4. Optionally humanize units.
// 5. Extract toolbar info.
function buildMetricSectionData(
  ctx: GpuComputeContext,
  id: number,
  kernelFallbackName: string,
  metricsKV: Record<string, number | string>,
  launchDurNs: number,
): KernelMetricData {
  const terminology = ctx.terminologyRegistry.get(ctx.terminologyId);

  // Canonical metric getter — translates through terminology.
  const getMetric = (metricId: string): number | string | null => {
    const val = metricsKV[metricId];
    if (val === undefined || val === null) return 'n/a';
    const num = typeof val === 'number' ? val : Number(val);
    return Number.isFinite(num) ? num : val;
  };

  const kernelName =
    getMetric('kernel_demangled_name') !== 'n/a'
      ? getMetric('kernel_demangled_name')
      : kernelFallbackName;
  const gridSize = `(${getMetric('launch__grid_size_x')}, ${getMetric('launch__grid_size_y')}, ${getMetric('launch__grid_size_z')})`;
  const blockSize = `(${getMetric('launch__block_size_x')}, ${getMetric('launch__block_size_y')}, ${getMetric('launch__block_size_z')})`;
  const launchConfig = `${gridSize}x${blockSize}`;

  // Build the set of available (non-n/a) metric IDs for visibility checks.
  // Include both trace-level keys and reverse-mapped canonical IDs so
  // section plugins can check their declared metric IDs directly.
  const availableMetrics = new Set<string>();
  for (const [key, val] of Object.entries(metricsKV)) {
    if (val !== 'n/a' && val !== null && val !== undefined) {
      availableMetrics.add(key);
    }
  }

  // Materialize MetricSection[] from the declarative Section registry.
  // A table is visible when all its 'required' rows have data.
  // A section is visible if at least one of its tables is visible.
  let sections: MetricSection[] = ctx.sectionRegistry
    .getSections()
    .map((section) => ({
      section: section.title,
      tables: section.tables
        .filter((tableDecl) => isTableVisible(tableDecl, availableMetrics))
        .map((tableDecl) => ({
          table_desc: tableDecl.description(terminology),
          data: tableDecl.rows.map(
            (row): MetricRow => ({
              metric_id: row.id,
              metric_label: row.label(terminology),
              metric_unit: row.unit(terminology),
              metric_value: getMetric(row.id),
            }),
          ),
        })),
    }))
    .filter((section) => section.tables.length > 0);

  if (ctx.humanizeMetrics) {
    sections = humanizeSections(sections, terminology);
  }

  // Extract toolbar info using well-known metric roles so the correct
  // vendor-specific metric is used (e.g. CUDA vs AMD).
  // Fall back to the renderstage slice duration when no counter metric
  // is available (e.g. traces without profiling counters).
  const durationId = ctx.sectionRegistry.getWellKnownMetricId(
    'duration',
    availableMetrics,
  );
  const cyclesId = ctx.sectionRegistry.getWellKnownMetricId(
    'cycles',
    availableMetrics,
  );
  const freqId = ctx.sectionRegistry.getWellKnownMetricId(
    'frequency',
    availableMetrics,
  );

  const rawDuration = (() => {
    if (durationId) {
      const counterDuration = getMetric(durationId);
      if (counterDuration !== null && counterDuration !== 'n/a') {
        return counterDuration;
      }
    }
    return Number.isFinite(launchDurNs) && launchDurNs > 0 ? launchDurNs : null;
  })();
  const rawCycles = cyclesId ? getMetric(cyclesId) : null;
  const rawFreq = freqId ? getMetric(freqId) : null;

  const formatToolbarMetric = (
    raw: number | string | null,
    unit: string,
  ): string => {
    if (raw === null || raw === 'n/a') return 'n/a';
    if (typeof raw === 'number' && Number.isFinite(raw)) {
      if (ctx.humanizeMetrics) {
        const humanized = humanizeRow(
          {
            metric_id: '',
            metric_label: '',
            metric_unit: unit,
            metric_value: raw,
          },
          terminology,
        );
        return `${Number(humanized.metric_value).toFixed(2)} ${humanized.metric_unit}`;
      }
      return `${raw.toFixed(2)} ${unit}`;
    }
    return String(raw);
  };

  return {
    id,
    kernelName: `Launch: ${kernelName} ${launchConfig}`,
    sections,
    toolbar: {
      sizeText: launchConfig,
      timeText: formatToolbarMetric(rawDuration, 'nsecond'),
      cyclesText:
        rawCycles !== null && rawCycles !== 'n/a' ? `${rawCycles}` : 'n/a',
      archText: `${getMetric('arch')}`,
      smFrequencyText: formatToolbarMetric(rawFreq, 'hz'),
      processText: `[${getMetric('process_id') ?? 'n/a'}] ${getMetric('process_name') ?? 'n/a'}`,
    },
  };
}

// Loads the full metric data for a single kernel slice.
//
// Executes the kernel query, reduces the rows, and builds
// {@link KernelMetricData} for each kernel group (sorted by launch time).
export async function fetchSelectedKernelMetricData(
  ctx: GpuComputeContext,
  engine: QueryCapable,
  sliceId: number,
): Promise<KernelMetricData[] | undefined> {
  const kernelQuery = buildKernelQuery(ctx, sliceId);
  const kernelResult = await engine.query(kernelQuery);
  const groups = reduceKernelRows(ctx, kernelResult.iter({}));
  return Array.from(groups.values())
    .sort((a, b) => a.launchTs - b.launchTs)
    .map((g) =>
      buildMetricSectionData(
        ctx,
        g.kernelId,
        g.kernelName,
        g.metricsKV,
        g.launchDur,
      ),
    );
}

// =============================================================================
// UI rendering helpers
// =============================================================================

const WarnIconClickPopup: m.Component = {
  view: ({children}) =>
    m(
      Popup,
      {
        offset: 6,
        fitContent: true,
        trigger: m(
          'span.pf-gpu-compute__warn-trigger',
          m(Icon, {icon: Icons.Warning, intent: Intent.Warning}),
        ),
      },
      children,
    ),
};

// Renders a two-column metric table from a {@link MetricTable}.
//
// Rows are split into left/right pairs to fill the 4-column layout.
// Percent-unit cells are rendered via the optional `percentBar` renderer,
// and baseline differences are resolved via `baselineLookup`.
export function renderMetricResultTable(
  ctx: GpuComputeContext,
  table: MetricTable,
  opts?: {
    percentBar?: PercentBarRenderer;
    baselineLookup?: Map<string, {unit: string; value: number | string}>;
  },
): m.Children {
  const baselineResolver = (
    curName: string,
    curUnit: string,
  ): number | string | null | undefined => {
    const terminology = ctx.terminologyRegistry.get(ctx.terminologyId);

    const entry = opts?.baselineLookup?.get(curName);
    if (!entry) return undefined;

    const baseVal = entry.value;
    const baseUnit = entry.unit;

    if (typeof baseVal === 'number') {
      return convertUnit(baseVal, baseUnit, curUnit, terminology) ?? null;
    }

    if (typeof baseVal === 'string') {
      return baseVal;
    }

    return undefined;
  };

  const renderCell = renderFormattedCell(opts?.percentBar, baselineResolver);

  const rows = table.data;
  type Row = (typeof rows)[number];
  const pairs: Array<[Row, Row?]> = [];
  for (let i = 0; i < Math.ceil(rows.length / 2); i += 1) {
    pairs.push([rows[i], rows[Math.ceil(rows.length / 2) + i]]);
  }

  return m('table.pf-gpu-compute__metric-table', [
    m('caption.pf-gpu-compute__metric-table-caption', table.table_desc),
    m('colgroup', [
      m('col.pf-gpu-compute__metric-col'),
      m('col.pf-gpu-compute__metric-col'),
      m('col.pf-gpu-compute__metric-col'),
      m('col.pf-gpu-compute__metric-col'),
    ]),
    m(
      'tbody',
      pairs.map(([left, right]) => {
        let leftMetricAndUnit = `${left.metric_label} [${left.metric_unit}]`;
        if (left.metric_unit === '') {
          leftMetricAndUnit = `${left.metric_label}`;
        }

        let rightMetricAndUnit = '';
        if (right !== undefined) {
          rightMetricAndUnit = `${right.metric_label} [${right.metric_unit}]`;
          if (right.metric_unit === '') {
            rightMetricAndUnit = `${right.metric_label}`;
          }
        }

        return m('tr.pf-gpu-compute__metric-row', [
          m('td.pf-gpu-compute__metric-cell', leftMetricAndUnit),
          m(
            'td.pf-gpu-compute__metric-value',
            renderCell(left.metric_label, left.metric_unit, left.metric_value),
          ),
          right
            ? m('td.pf-gpu-compute__metric-cell--right', rightMetricAndUnit)
            : m('td.pf-gpu-compute__metric-cell--right'),
          right
            ? m(
                'td.pf-gpu-compute__metric-value',
                renderCell(
                  right.metric_label,
                  right.metric_unit,
                  right.metric_value,
                ),
              )
            : m('td.pf-gpu-compute__metric-value'),
        ]);
      }),
    ),
  ]);
}

// Renders a list of collapsible sections, each containing its tables.
// Generic over the table type so it can be reused by different renderers.
export function renderSectionList<TableType>(
  sections: {section: string; tables: TableType[]}[],
  opts: {
    keyPrefix: string;
    renderTable: (table: TableType) => m.Children;
    renderSectionFooter?: (section: {
      section: string;
      tables: TableType[];
    }) => m.Children;
    isCollapsed?: (sectionName: string) => boolean;
  },
): m.Children {
  return m(Accordion, {multi: true}, [
    sections.map((sec) => {
      const defaultOpen = !(opts.isCollapsed?.(sec.section) ?? false);
      return m(AccordionSection, {summary: sec.section, defaultOpen}, [
        m('div', [
          ...sec.tables.map(opts.renderTable),
          opts.renderSectionFooter?.(sec),
        ]),
      ]);
    }),
  ]);
}

// Returns a cell-renderer function that applies baseline diffs,
// percent bars, and warning icons depending on the metric's unit.
export function renderFormattedCell(
  percentBar?: PercentBarRenderer,
  baselineOf?: (
    name: string,
    unit: string,
  ) => number | string | null | undefined,
) {
  return (name: string, unit: string, val: number | string | null) => {
    if (val == null) {
      return 'n/a';
    }

    const baseline = baselineOf?.(name, unit);

    const curValue = typeof val === 'number' ? formatNumber(val) : String(val);

    if (baseline === null) {
      return m('span.pf-gpu-compute__inline-flex', [
        m('span.pf-gpu-compute__inline-flex', [
          m(WarnIconClickPopup, 'Baseline unit not comparable'),
          curValue,
        ]),
      ]);
    }

    if (unit.includes('%') && percentBar && typeof val === 'number') {
      const baseNum =
        typeof baseline === 'number' && Number.isFinite(baseline)
          ? baseline
          : null;
      return percentBar(val, baseNum);
    }

    let diffPoints = '';
    let diffPct = '';
    if (
      typeof baseline === 'number' &&
      Number.isFinite(baseline) &&
      typeof val === 'number' &&
      Number.isFinite(val)
    ) {
      const points = val - baseline;
      const signedPoints = (points >= 0 ? '+' : '') + formatNumber(points, 2);
      diffPoints = ` (${signedPoints})`;

      if (baseline === 0) {
        diffPct = points === 0 ? ' (+0%)' : ` (${points >= 0 ? '+' : '-'}inf%)`;
      } else {
        const pct = (points / baseline) * 100;
        const signedPct = (pct >= 0 ? '+' : '') + formatNumber(pct, 2);
        diffPct = ` (${signedPct}%)`;
      }
    } else if (
      typeof baseline === 'string' &&
      typeof val === 'string' &&
      baseline !== val
    ) {
      diffPoints = ' → ' + baseline;
    }

    return m('span', [curValue, diffPoints, diffPct]);
  };
}

// =============================================================================
// KernelMetricsSection component
// =============================================================================

// Attrs accepted by the top-level {@link KernelMetricsSection} component.
export interface KernelMetricsSectionSettings {
  ctx: GpuComputeContext;
  engine: QueryCapable;
  sliceId?: number;
  renderKernel?: (
    kernel: KernelMetricData,
    renderCtx: {engine: QueryCapable},
  ) => m.Children | undefined;
  baseline?: KernelMetricData;
  analysisCache?: AnalysisCache;
}

// Internal state for {@link KernelMetricsSection}.
type DataState = {
  kernelTableData?: KernelMetricData[];
  loadedSliceId?: number;
  loadedTerminologyId?: string;
};

function loadMetricData(
  attrs: KernelMetricsSectionSettings,
  state: DataState,
): void {
  state.loadedSliceId = attrs.sliceId;
  state.loadedTerminologyId = attrs.ctx.terminologyId;

  const findFirstWithMetrics = async (): Promise<KernelMetricData[]> => {
    const sql = `
      SELECT s.id
      FROM gpu_slice s
      INNER JOIN gpu_track tr ON tr.id = s.track_id
      INNER JOIN counter c ON c.ts >= s.ts AND c.ts < s.ts + s.dur
        AND c.track_id IN (
          SELECT gc_tc.id FROM gpu_counter_track gc_tc
          INNER JOIN gpu_counter_group gc ON gc.track_id = gc_tc.id
            AND gc.group_id = ${COMPUTE_COUNTER_GROUP_ID}
        )
      WHERE s.render_stage_category = ${COMPUTE_RENDER_STAGE_CATEGORY}
      LIMIT 1;
    `;
    const result = await attrs.engine.query(sql);
    const iter = result.iter({});
    if (!iter.valid()) return [];
    const firstId = Number(iter.get('id'));
    return (
      (await fetchSelectedKernelMetricData(attrs.ctx, attrs.engine, firstId)) ??
      []
    );
  };

  const load = async () => {
    let data: KernelMetricData[] = [];
    if (attrs.sliceId != null) {
      data =
        (await fetchSelectedKernelMetricData(
          attrs.ctx,
          attrs.engine,
          attrs.sliceId,
        )) ?? [];
      if (data.length === 0) {
        data = await findFirstWithMetrics();
      }
    } else {
      data = await findFirstWithMetrics();
    }
    state.kernelTableData = data;
  };

  load();
}

// Top-level Mithril component for the "Details" tab.
//
// Loads the metric data for the selected slice (or finds the first
// kernel with metrics), builds section collapse state, and renders
// the full metric table tree. Re-fetches when sliceId or terminology
// changes.
export const KernelMetricsSection: m.Component<
  KernelMetricsSectionSettings,
  DataState
> = {
  oninit: ({attrs, state}) => {
    loadMetricData(attrs, state);
  },

  onbeforeupdate: ({attrs, state}) => {
    if (
      attrs.sliceId !== state.loadedSliceId ||
      attrs.ctx.terminologyId !== state.loadedTerminologyId
    ) {
      loadMetricData(attrs, state);
    }
  },

  view: ({attrs, state}) => {
    if (!state.kernelTableData) return null;

    const baselineLookup:
      | Map<string, {unit: string; value: number | string}>
      | undefined = (() => {
      const baseline = attrs.baseline;
      if (!baseline) return undefined;

      const map = new Map<string, {unit: string; value: number | string}>();
      for (const section of baseline.sections) {
        for (const table of section.tables) {
          for (const row of table.data) {
            const val = row.metric_value;
            if (typeof val === 'number' || typeof val === 'string') {
              map.set(row.metric_label, {unit: row.metric_unit, value: val});
            }
          }
        }
      }
      return map;
    })();

    const renderSingleTable = (kernel: KernelMetricData) => {
      const renderer = (table: MetricTable) =>
        renderMetricResultTable(attrs.ctx, table, {
          percentBar: renderPercentBar,
          baselineLookup,
        });

      if (kernel.sections.length === 0) {
        return m(
          '.pf-gpu-compute__pad',
          m('p', 'No detailed metrics available for this kernel.'),
        );
      }

      return (
        (attrs.renderKernel &&
          attrs.renderKernel(kernel, {engine: attrs.engine})) ??
        (() => {
          const analysisProvider = attrs.ctx.analysisProviderHolder.get();
          const renderFooter =
            attrs.analysisCache && analysisProvider
              ? (sec: MetricSection) =>
                  analysisProvider.renderSectionAnalysis({
                    section: sec,
                    kernelData: kernel,
                    sliceId: kernel.id,
                    analysisCache: attrs.analysisCache!,
                  })
              : undefined;
          const sections = attrs.ctx.sectionRegistry.getSections();
          return renderSectionList(kernel.sections, {
            keyPrefix: `${kernel.id}:`,
            renderTable: renderer,
            renderSectionFooter: renderFooter,
            isCollapsed: (name) =>
              sections.find((s) => s.title === name)?.collapsedByDefault ??
              false,
          });
        })()
      );
    };

    if (state.kernelTableData.length === 0) {
      return m(
        '.pf-gpu-compute__pad',
        m('p', 'No kernel compute metrics for this trace.'),
      );
    }

    return m(
      '.pf-gpu-compute__pad',
      renderSingleTable(state.kernelTableData[0]),
    );
  },
};
