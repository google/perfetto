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

// Summary table for the GPU Compute tab.
//
// Shows a sortable, double-click-navigable list of every compute kernel
// launch in the trace.  Each row contains the kernel's demangled name,
// duration, compute/memory throughput, register count, and grid size
// rendered as relative percent-bars so hot kernels stand out visually.

import m from 'mithril';
import {getTerminology} from './terminology';
import type {Engine} from '../../trace_processor/engine';
import {
  renderPercentBar,
  formatNumber,
  COMPUTE_RENDER_STAGE_CATEGORY,
} from './details';
import {Icons} from '../../base/semantic_icons';
import {Icon} from '../../widgets/icon';
import type {GpuComputeContext} from './index';
import {adjustSeconds} from './humanize';
import {getWellKnownMetricIds} from './section';

// Per-kernel row returned by {@link fetchKernelSummaryRows}.
type SummaryRow = {
  id: number;
  demangledName: string;
  durationNSecNum: number | string | null;
  computePct: number | string | null;
  memoryPct: number | string | null;
  registersPerThread: number | string | null;
  gridSize: number | string | null;
};

// Component state holding the fetched rows and per-column max values.
type SummaryState = {
  rows?: SummaryRow[];
  maxDurationNSec?: number;
  maxComputePct?: number;
  maxMemoryPct?: number;
  maxRegisters?: number;
  maxGridSize?: number;
  launchIndexBySliceId: Map<number, number>;
  sortKey: SortKey | null;
  sortDescending: boolean;
};

// Renders a bar whose width is proportional to `val / max`.
// Falls back to `—` when the value is non-finite or missing.
const renderRelPercentBar = (
  val?: number,
  max?: number,
  label?: string,
): m.Children => {
  const hasLabel = typeof label === 'string' && label.trim() !== '';
  const curVal = Number(val);
  const maxVal = Number(max);
  if (!Number.isFinite(curVal) || !Number.isFinite(maxVal) || maxVal <= 0) {
    return hasLabel ? renderPercentBar(0, null, false, label) : '—';
  }
  const clamped = Math.max(0, Math.min(curVal, maxVal));
  const pct = Math.max(0, Math.min(100, (clamped / maxVal) * 100));
  return renderPercentBar(pct, null, false, label ?? '');
};

// =============================================================================
// Data fetching
// =============================================================================

// Fetches one summary row per compute kernel launch.
//
// Uses a CTE to pre-join the three counter metrics (duration, compute
// throughput, memory throughput) in a single pass, then extracts
// launch-arg columns from the slice's arg set.
export async function fetchKernelSummaryRows(
  engine: Engine,
): Promise<SummaryRow[]> {
  const durationNames = getWellKnownMetricIds('duration');
  const computeNames = getWellKnownMetricIds('compute_throughput');
  const memoryNames = getWellKnownMetricIds('memory_throughput');
  const allNames = [...durationNames, ...computeNames, ...memoryNames];

  // Build the IN clause for counter name filtering.
  const inClause = allNames.map((n) => `'${n}'`).join(', ');

  // Build COALESCE expression that picks the first available metric per role.
  const coalesceExpr = (names: string[]): string => {
    if (names.length === 0) return 'NULL';
    const parts = names.map(
      (n) =>
        `(SELECT cv.value FROM counter_vals cv WHERE cv.slice_id = cs.id AND cv.metric_name = '${n}' LIMIT 1)`,
    );
    return names.length === 1 ? parts[0] : `COALESCE(${parts.join(', ')})`;
  };

  const sql = `
    WITH compute_slices AS (
      SELECT s.id, s.ts, s.dur, s.arg_set_id, s.name
      FROM gpu_slice s
      INNER JOIN gpu_track tr ON tr.id = s.track_id
      WHERE s.render_stage_category = ${COMPUTE_RENDER_STAGE_CATEGORY}
    ),
    counter_vals AS (
      SELECT
        cs.id AS slice_id,
        tc.name AS metric_name,
        c.value
      FROM compute_slices cs
      INNER JOIN counter c ON c.ts >= cs.ts AND (cs.dur IS NULL OR c.ts < cs.ts + cs.dur)
      INNER JOIN gpu_counter_track tc ON tc.id = c.track_id
        AND tc.name IN (${inClause})
      WHERE c.value <> 0
    )
    SELECT
      cs.id AS id,
      COALESCE(
        EXTRACT_ARG(cs.arg_set_id, 'kernel_demangled_name'),
        EXTRACT_ARG(cs.arg_set_id, 'kernel_name'),
        cs.name
      ) AS demangledName,
      CAST(EXTRACT_ARG(cs.arg_set_id, 'launch__registers_per_thread') AS REAL) AS launch__registers_per_thread,
      CAST(EXTRACT_ARG(cs.arg_set_id, 'launch__grid_size') AS REAL) AS launch__grid_size,
      COALESCE(
        ${coalesceExpr(durationNames)},
        CAST(cs.dur AS REAL)
      ) AS durationNSecNum,
      ${coalesceExpr(computeNames)} AS computePct,
      ${coalesceExpr(memoryNames)} AS memoryPct
    FROM compute_slices cs
    ORDER BY cs.ts ASC;
  `;

  const result = await engine.query(sql);
  const iter = result.iter({});
  const list: SummaryRow[] = [];
  while (iter.valid()) {
    list.push({
      id: Number(iter.get('id')),
      demangledName: String(iter.get('demangledName') ?? ''),
      durationNSecNum: (iter.get('durationNSecNum') as number | null) ?? null,
      computePct: (iter.get('computePct') as number | string | null) ?? null,
      memoryPct: (iter.get('memoryPct') as number | string | null) ?? null,
      registersPerThread:
        (iter.get('launch__registers_per_thread') as number | string | null) ??
        null,
      gridSize:
        (iter.get('launch__grid_size') as number | string | null) ?? null,
    });
    iter.next();
  }
  return list;
}

// =============================================================================
// Summary section component
// =============================================================================

// Attrs accepted by {@link KernelSummarySection}.
export interface SummarySectionAttrs extends m.Attributes {
  ctx: GpuComputeContext;
  engine: Engine;
  sliceId?: number;
  openSliceInDetail?: (sliceId: number) => void;
}

// Column keys that the table can be sorted by.
type SortKey =
  | 'id'
  | 'name'
  | 'duration'
  | 'compute'
  | 'memory'
  | 'registers'
  | 'grid_size';

// Returns the sortable primitive for `key` from a row.
function getSortableValue(
  r: SummaryRow,
  key: SortKey,
  launchIndex: Map<number, number>,
): number | string | undefined {
  switch (key) {
    case 'id':
      return launchIndex.get(r.id) ?? r.id;
    case 'name':
      return r.demangledName ?? '';
    case 'duration':
      return Number(r.durationNSecNum);
    case 'compute':
      return Number(r.computePct);
    case 'memory':
      return Number(r.memoryPct);
    case 'registers':
      return Number(r.registersPerThread);
    case 'grid_size':
      return Number(r.gridSize);
  }
}

// Three-way comparator for summary rows.
//
// Non-finite / missing values are pushed to the edge of the sort
// (bottom for descending, top for ascending) so real data stays grouped.
function compare(
  a: SummaryRow,
  b: SummaryRow,
  key: SortKey,
  descending: boolean,
  launchIndex: Map<number, number>,
): number {
  const aVal = getSortableValue(a, key, launchIndex);
  const bVal = getSortableValue(b, key, launchIndex);

  const isANum = typeof aVal === 'number' && Number.isFinite(aVal);
  const isBNum = typeof bVal === 'number' && Number.isFinite(bVal);

  // Push non-values to the edge so real data stays grouped
  const aUndef = aVal == null || (typeof aVal === 'number' && !isANum);
  const bUndef = bVal == null || (typeof bVal === 'number' && !isBNum);
  if (aUndef !== bUndef) {
    return (aUndef ? 1 : -1) * (descending ? 1 : -1);
  }

  // Numeric comparison
  if (isANum && isBNum) {
    const delta = Number(aVal) - Number(bVal);
    return descending ? -Math.sign(delta) : Math.sign(delta);
  }

  // String comparison fallback
  const aStr = String(aVal ?? '');
  const bStr = String(bVal ?? '');
  return descending ? -aStr.localeCompare(bStr) : aStr.localeCompare(bStr);
}

// Mithril component that renders the summary table.
//
// On init it fetches all kernel launches via {@link fetchKernelSummaryRows},
// computes per-column max values for the relative bars, and renders a
// sortable `<table>` whose rows can be double-clicked to navigate to
// the kernel's detail view.
export const KernelSummarySection: m.Component<
  SummarySectionAttrs,
  SummaryState
> = {
  async oninit({attrs, state}) {
    state.launchIndexBySliceId = new Map();
    state.sortKey = 'id';
    state.sortDescending = false;

    const rows = await fetchKernelSummaryRows(attrs.engine);

    // Build launch-order map so the ID column shows 0, 1, 2, …
    rows.forEach((opt, zeroBasedIndex) =>
      state.launchIndexBySliceId.set(opt.id, zeroBasedIndex),
    );

    // Initial sort by launch order (ascending)
    rows.sort((a, b) => a.id - b.id);

    // Per-column max values drive the relative percent-bar widths
    const finiteMax = (arr: Array<number | null | undefined>) => {
      const nums = arr
        .map(Number)
        .filter((x) => Number.isFinite(x)) as number[];
      if (nums.length === 0) {
        return undefined;
      }
      return Math.max(...nums);
    };

    state.rows = rows;
    state.maxDurationNSec = finiteMax(
      rows.map((r) => Number(r.durationNSecNum)),
    );
    state.maxComputePct = finiteMax(rows.map((r) => Number(r.computePct)));
    state.maxMemoryPct = finiteMax(rows.map((r) => Number(r.memoryPct)));
    state.maxRegisters = finiteMax(
      rows.map((r) => Number(r.registersPerThread)),
    );
    state.maxGridSize = finiteMax(rows.map((r) => Number(r.gridSize)));

    m.redraw();
  },

  view({state, attrs}) {
    const terminology = getTerminology(attrs.ctx.terminologyId);

    const rows = state.rows ?? [];

    // Formats a raw metric value into a display label with optional unit.
    const label = (
      val: number | string | null | undefined,
      unit?: string,
    ): string => {
      if (val == null || val === 'null' || val === 'undefined') {
        return '—';
      }

      // Humanize seconds when enabled
      if (unit === 'nsecond' && Number.isFinite(Number(val))) {
        if (attrs.ctx.humanizeMetrics) {
          const {value: v, unit: u} = adjustSeconds(Number(val) / 1e9);
          return `${formatNumber(v)} ${u}`;
        }
        return `${formatNumber(Number(val))} nsecond`;
      }

      const text = Number.isFinite(Number(val))
        ? String(formatNumber(Number(val)))
        : String(val);
      return unit ? `${text} ${unit}` : text;
    };

    // Sort rows immutably for rendering
    const {sortKey, sortDescending, launchIndexBySliceId} = state;
    const sortedRows = (() => {
      if (!sortKey) return rows;
      const copy = rows.slice();
      copy.sort((a, b) =>
        compare(a, b, sortKey, sortDescending, launchIndexBySliceId),
      );
      return copy;
    })();

    // Cycle sort direction on header click
    const onSort = (key: SortKey) => {
      if (state.sortKey === key) {
        state.sortDescending = !state.sortDescending;
      } else {
        state.sortKey = key;
        state.sortDescending = true;
      }
      m.redraw();
    };

    // Up/down arrow indicator for the active sort column
    const arrowIconFor = (key: SortKey) => {
      if (state.sortKey !== key) return null;
      const icon = state.sortDescending ? 'expand_more' : 'expand_less';
      return m(
        'i',
        {class: 'pf-icon pf-left-icon', style: 'margin-left:6px;'},
        icon,
      );
    };

    const headerCell = (text: string, key: SortKey) =>
      m(
        'th.pf-gpu-compute__summary-th',
        {
          onclick: () => onSort(key),
          title: 'Sort',
        },
        [text, arrowIconFor(key)],
      );

    return m(
      '.pf-gpu-compute',
      m('table.pf-gpu-compute__summary-table', [
        m('caption.pf-gpu-compute__summary-caption', [
          m('.pf-gpu-compute__summary-caption-row', [
            m(Icon, {
              icon: Icons.Help,
              title: 'About this table',
              style: 'font-size:16px;',
            }),
            m(
              'span',
              'This table shows all results in the report. Use the column headers to sort the results in this report. Double-Click a result to see detailed metrics.',
            ),
          ]),
        ]),

        m('colgroup', [
          m('col', {style: 'width:5%'}),
          m('col', {style: 'width:25%'}),
          m('col', {style: 'width:14%'}),
          m('col', {style: 'width:14%'}),
          m('col', {style: 'width:14%'}),
          m('col', {style: 'width:14%'}),
          m('col', {style: 'width:14%'}),
        ]),

        m(
          'thead',
          m('tr.pf-gpu-compute__summary-thead-row', [
            headerCell('ID', 'id'),
            headerCell('Demangled Name', 'name'),
            headerCell('Duration', 'duration'),
            headerCell('Compute Throughput', 'compute'),
            headerCell('Memory Throughput', 'memory'),
            headerCell('# Registers', 'registers'),
            headerCell(`${terminology.grid.title} Size`, 'grid_size'),
          ]),
        ),

        m(
          'tbody',
          sortedRows.map((r) =>
            m(
              'tr.pf-gpu-compute__summary-row',
              {
                ondblclick: () => attrs.openSliceInDetail?.(r.id),
              },
              [
                m(
                  'td.pf-gpu-compute__summary-td',
                  String(launchIndexBySliceId.get(r.id) ?? r.id),
                ),
                m(
                  'td.pf-gpu-compute__summary-td.pf-gpu-compute__summary-td--name',
                  {title: r.demangledName},
                  r.demangledName,
                ),
                m(
                  'td.pf-gpu-compute__summary-td',
                  renderRelPercentBar(
                    Number(r.durationNSecNum),
                    state.maxDurationNSec,
                    label(Number(r.durationNSecNum), 'nsecond'),
                  ),
                ),
                m(
                  'td.pf-gpu-compute__summary-td',
                  renderRelPercentBar(
                    Number(r.computePct),
                    state.maxComputePct,
                    label(r.computePct),
                  ),
                ),
                m(
                  'td.pf-gpu-compute__summary-td',
                  renderRelPercentBar(
                    Number(r.memoryPct),
                    state.maxMemoryPct,
                    label(r.memoryPct),
                  ),
                ),
                m(
                  'td.pf-gpu-compute__summary-td',
                  renderRelPercentBar(
                    Number(r.registersPerThread),
                    state.maxRegisters,
                    label(r.registersPerThread),
                  ),
                ),
                m(
                  'td.pf-gpu-compute__summary-td',
                  renderRelPercentBar(
                    Number(r.gridSize),
                    state.maxGridSize,
                    label(r.gridSize),
                  ),
                ),
              ],
            ),
          ),
        ),
      ]),
    );
  },
};
