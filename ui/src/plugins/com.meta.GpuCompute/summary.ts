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
import type {Engine} from '../../trace_processor/engine';
import {
  renderPercentBar,
  formatNumber,
  COMPUTE_RENDER_STAGE_CATEGORY,
} from './details';
import {Icons} from '../../base/semantic_icons';
import {Button} from '../../widgets/button';
import {Icon} from '../../widgets/icon';
import type {GpuComputeContext} from './index';
import {adjustSeconds} from './humanize';
import {NUM, NUM_NULL, STR_NULL} from '../../trace_processor/query_result';
import {assertUnreachable} from '../../base/assert';

// Per-kernel row returned by {@link fetchKernelSummaryRows}.
export type SummaryRow = {
  readonly id: number;
  readonly demangledName: string;
  readonly durationNSecNum: number;
  readonly computePct: number;
  readonly memoryPct: number;
  readonly registersPerThread: number;
  readonly gridSize: number;
};

const PAGE_SIZE = 100;

// Renders a bar whose width is proportional to `val / max`.
// Falls back to `—` when the value is non-finite or missing.
function renderRelPercentBar(
  val: number,
  max?: number, // Undefined means no max
  label?: string,
): m.Children {
  const hasLabel = label !== undefined && label.trim() !== '';
  const curVal = val;
  const maxVal = Number(max); // NaN if undefined
  if (!Number.isFinite(curVal) || !Number.isFinite(maxVal) || maxVal <= 0) {
    return hasLabel ? renderPercentBar(0, null, false, label) : '—';
  }
  const clamped = Math.max(0, Math.min(curVal, maxVal));
  const pct = Math.max(0, Math.min(100, (clamped / maxVal) * 100));
  return renderPercentBar(pct, null, false, label ?? '');
}

// =============================================================================
// Data fetching
// =============================================================================

// Fetches one summary row per compute kernel launch.
//
// Uses a CTE to pre-join the three counter metrics (duration, compute
// throughput, memory throughput) in a single pass, then extracts
// launch-arg columns from the slice's arg set.
export async function fetchKernelSummaryRows(
  ctx: GpuComputeContext,
  engine: Engine,
): Promise<SummaryRow[]> {
  const durationNames = ctx.sectionRegistry.getWellKnownMetricIds('duration');
  const computeNames =
    ctx.sectionRegistry.getWellKnownMetricIds('compute_throughput');
  const memoryNames =
    ctx.sectionRegistry.getWellKnownMetricIds('memory_throughput');
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
      CAST(EXTRACT_ARG(cs.arg_set_id, 'registers_per_thread') AS REAL) AS registersPerThread,
      CAST(EXTRACT_ARG(cs.arg_set_id, 'launch.grid_size.x') AS REAL)
        * CAST(COALESCE(EXTRACT_ARG(cs.arg_set_id, 'launch.grid_size.y'), 1) AS REAL)
        * CAST(COALESCE(EXTRACT_ARG(cs.arg_set_id, 'launch.grid_size.z'), 1) AS REAL) AS gridSize,
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
  const iter = result.iter({
    id: NUM,
    demangledName: STR_NULL,
    durationNSecNum: NUM_NULL,
    // The follwoing can be string | number | null - we don't have a type for
    // this - just use unknown and cast later
    computePct: NUM_NULL,
    memoryPct: NUM_NULL,
    registersPerThread: NUM_NULL,
    gridSize: NUM_NULL,
  });
  const list: SummaryRow[] = [];
  while (iter.valid()) {
    list.push({
      id: iter.id,
      demangledName: iter.demangledName ?? '',
      durationNSecNum: iter.durationNSecNum ?? 0,
      computePct: iter.computePct ?? 0,
      memoryPct: iter.memoryPct ?? 0,
      registersPerThread: iter.registersPerThread ?? 0,
      gridSize: iter.gridSize ?? 0,
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
  readonly ctx: GpuComputeContext;
  readonly engine: Engine;
  readonly sliceId?: number;
  readonly openSliceInDetail?: (sliceId: number) => void;
  readonly prefetchedRows: readonly SummaryRow[];
}

// Column keys that the table can be sorted by.
type SortKey =
  'id' | 'name' | 'duration' | 'compute' | 'memory' | 'registers' | 'grid_size';

// Returns the sortable primitive for `key` from a row.
function getSortableValue(
  r: SummaryRow,
  key: SortKey,
  launchIndex: ReadonlyMap<number, number>,
): number | string {
  switch (key) {
    case 'id':
      return launchIndex.get(r.id) ?? r.id;
    case 'name':
      return r.demangledName;
    case 'duration':
      return r.durationNSecNum;
    case 'compute':
      return r.computePct;
    case 'memory':
      return r.memoryPct;
    case 'registers':
      return r.registersPerThread;
    case 'grid_size':
      return r.gridSize;
    default:
      assertUnreachable(key);
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
    const delta = aVal - bVal;
    return descending ? -Math.sign(delta) : Math.sign(delta);
  }

  // String comparison fallback
  const aStr = String(aVal ?? '');
  const bStr = String(bVal ?? '');
  return descending ? -aStr.localeCompare(bStr) : aStr.localeCompare(bStr);
}

// Mithril component that renders the summary table.
//
// Renders a sortable `<table>` whose rows can be double-clicked to navigate to
// the kernel's detail view.
export function KernelSummarySection({
  attrs,
}: m.Vnode<SummarySectionAttrs>): m.Component<SummarySectionAttrs> {
  const launchIndexBySliceId = new Map();
  let sortKey: SortKey = 'id';
  let sortDescending = false;
  let pageOffset = 0;

  const rows = attrs.prefetchedRows;

  // Build launch-order map so the ID column shows 0, 1, 2, …
  rows.forEach((opt, zeroBasedIndex) =>
    launchIndexBySliceId.set(opt.id, zeroBasedIndex),
  );

  // Per-column max values drive the relative percent-bar widths
  const finiteMax = (arr: number[]) => {
    const nums = arr.filter((x) => Number.isFinite(x));
    if (nums.length === 0) {
      return undefined;
    }
    // Reduce, not `Math.max(...nums)`, which overflows the stack for large arrays.
    return nums.reduce((a, b) => Math.max(a, b));
  };

  const maxDurationNSec = finiteMax(rows.map((r) => r.durationNSecNum));
  const maxComputePct = finiteMax(rows.map((r) => r.computePct));
  const maxMemoryPct = finiteMax(rows.map((r) => r.memoryPct));
  const maxRegisters = finiteMax(rows.map((r) => r.registersPerThread));
  const maxGridSize = finiteMax(rows.map((r) => r.gridSize));

  return {
    view({attrs}) {
      const terminology = attrs.ctx.terminologyRegistry.get(
        attrs.ctx.terminologyId,
      );

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
        if (sortKey === key) {
          sortDescending = !sortDescending;
        } else {
          sortKey = key;
          sortDescending = true;
        }
        pageOffset = 0;
      };

      // Up/down arrow indicator for the active sort column
      const arrowIconFor = (key: SortKey) => {
        if (sortKey !== key) return null;
        const icon = sortDescending ? 'expand_more' : 'expand_less';
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
            sortedRows.slice(pageOffset, pageOffset + PAGE_SIZE).map((r) =>
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
                      r.durationNSecNum,
                      maxDurationNSec,
                      label(r.durationNSecNum, 'nsecond'),
                    ),
                  ),
                  m(
                    'td.pf-gpu-compute__summary-td',
                    renderRelPercentBar(
                      r.computePct,
                      maxComputePct,
                      label(r.computePct),
                    ),
                  ),
                  m(
                    'td.pf-gpu-compute__summary-td',
                    renderRelPercentBar(
                      r.memoryPct,
                      maxMemoryPct,
                      label(r.memoryPct),
                    ),
                  ),
                  m(
                    'td.pf-gpu-compute__summary-td',
                    renderRelPercentBar(
                      r.registersPerThread,
                      maxRegisters,
                      label(r.registersPerThread),
                    ),
                  ),
                  m(
                    'td.pf-gpu-compute__summary-td',
                    renderRelPercentBar(
                      r.gridSize,
                      maxGridSize,
                      label(r.gridSize),
                    ),
                  ),
                ],
              ),
            ),
          ),
        ]),
        sortedRows.length > PAGE_SIZE &&
          m('.pf-gpu-compute__summary-pagination', [
            m(Button, {
              icon: Icons.PrevPage,
              disabled: pageOffset === 0,
              onclick: () => {
                pageOffset = Math.max(0, pageOffset - PAGE_SIZE);
              },
            }),
            m(
              'span',
              `${pageOffset + 1}–${Math.min(pageOffset + PAGE_SIZE, sortedRows.length)} of ${sortedRows.length}`,
            ),
            m(Button, {
              icon: Icons.NextPage,
              disabled: pageOffset + PAGE_SIZE >= sortedRows.length,
              onclick: () => {
                pageOffset = Math.min(
                  pageOffset + PAGE_SIZE,
                  sortedRows.length - PAGE_SIZE,
                );
              },
            }),
          ]),
      );
    },
  };
}
