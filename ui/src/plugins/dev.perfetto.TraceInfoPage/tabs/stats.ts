// Copyright (C) 2025 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

import m from 'mithril';
import type {Engine} from '../../../trace_processor/engine';
import {
  LONG,
  NUM,
  NUM_NULL,
  type InferRowType,
} from '../../../trace_processor/query_result';
import {formatNumber} from '../../../components/widgets/charts/chart_utils';
import type {
  HistogramBucket,
  HistogramData,
} from '../../../components/widgets/charts/histogram_loader';
import {HistogramSvg} from '../../../components/widgets/charts_svg/histogram_svg';
import {Anchor} from '../../../widgets/anchor';
import {Button} from '../../../widgets/button';
import {Card} from '../../../widgets/card';
import {Section} from '../../../widgets/section';
import {Grid, GridCell, GridHeaderCell} from '../../../widgets/grid';
import {Icon} from '../../../widgets/icon';
import {Stack} from '../../../widgets/stack';
import {Tooltip} from '../../../widgets/tooltip';
import {statsSpec} from '../utils';

// C++ producers (e.g. heapprofd) use std::numeric_limits<int64_t>::max() as the
// upper_bound sentinel for open-ended overflow buckets.
const INT64_MAX = 9223372036854775807n;

const statsRowSpec = {
  ...statsSpec,
  id: NUM,
};

type StatsRow = InferRowType<typeof statsRowSpec>;

export interface StatsData {
  allStats: StatsRow[];
  histograms: Map<number, HistogramData>;
  isMultiTrace: boolean;
  isMultiMachine: boolean;
}

export async function loadStatsData(engine: Engine): Promise<StatsData> {
  // Load all stats
  const allStatsResult = await engine.query(`
    select
      id,
      name,
      value,
      cast(ifnull(idx, '') as text) as idx,
      description,
      severity,
      source,
      machine_id as machineId,
      trace_id as traceId
    from stats
    order by trace_id, machine_id, name, idx
  `);
  const allStats: StatsRow[] = [];
  for (
    const iter = allStatsResult.iter(statsRowSpec);
    iter.valid();
    iter.next()
  ) {
    allStats.push({
      id: iter.id,
      name: iter.name,
      value: iter.value,
      description: iter.description,
      idx: iter.idx,
      severity: iter.severity,
      source: iter.source,
      machineId: iter.machineId,
      traceId: iter.traceId,
    });
  }

  // Load histogram buckets for non-empty histogram-type stats.
  const bucketsResult = await engine.query(`
    select
      b.stat_id as statId,
      b.lower_bound as lowerBound,
      b.upper_bound as upperBound,
      b.count,
      s.value as totalCount
    from stats_histogram_bucket b
    join stats s on s.id = b.stat_id
    where s.value > 0
    order by b.stat_id, b.lower_bound
  `);
  const bucketsByStatId = new Map<
    number,
    {totalCount: number; buckets: HistogramBucket[]}
  >();
  for (
    const iter = bucketsResult.iter({
      statId: NUM,
      lowerBound: LONG,
      upperBound: LONG,
      count: LONG,
      totalCount: NUM,
    });
    iter.valid();
    iter.next()
  ) {
    let entry = bucketsByStatId.get(iter.statId);
    if (entry === undefined) {
      entry = {totalCount: iter.totalCount, buckets: []};
      bucketsByStatId.set(iter.statId, entry);
    }
    entry.buckets.push({
      start: Number(iter.lowerBound),
      end:
        iter.upperBound >= INT64_MAX
          ? Number.POSITIVE_INFINITY
          : Number(iter.upperBound),
      count: Number(iter.count),
    });
  }

  const histograms = new Map<number, HistogramData>();
  for (const [statId, {totalCount, buckets}] of bucketsByStatId) {
    const first = buckets[0];
    const last = buckets[buckets.length - 1];
    if (first === undefined || last === undefined) continue;
    histograms.set(statId, {
      buckets,
      min: first.start,
      max: last.end,
      totalCount,
      nullCount: 0,
      nonNumericCount: 0,
    });
  }

  // Determine whether the trace spans multiple traces/machines so we know
  // whether to show the per-trace/per-machine columns. The metadata table is
  // the authoritative source of trace/machine ids; supplement it with any ids
  // observed only in the stats rows.
  const traceIds = new Set<number>();
  const machineIds = new Set<number>();

  const idResult = await engine.query(`
    select distinct trace_id as traceId, machine_id as machineId
    from metadata
  `);
  for (
    const iter = idResult.iter({traceId: NUM_NULL, machineId: NUM_NULL});
    iter.valid();
    iter.next()
  ) {
    if (iter.traceId !== null) {
      traceIds.add(iter.traceId);
    }
    if (iter.machineId !== null) {
      machineIds.add(iter.machineId);
    }
  }
  for (const stat of allStats) {
    if (stat.machineId !== null) {
      machineIds.add(stat.machineId);
    }
    if (stat.traceId !== null) {
      traceIds.add(stat.traceId);
    }
  }

  return {
    allStats,
    histograms,
    isMultiTrace: traceIds.size > 1,
    isMultiMachine: machineIds.size > 1,
  };
}

function statHistogramElementId(statId: number): string {
  return `stat-histogram-${statId}`;
}

function scrollToStatHistogram(statId: number): void {
  const element = document.getElementById(statHistogramElementId(statId));
  if (element) {
    element.scrollIntoView({behavior: 'smooth', block: 'start'});
  }
}

export interface StatsTabAttrs {
  data: StatsData;
}

export class StatsTab implements m.ClassComponent<StatsTabAttrs> {
  view({attrs}: m.CVnode<StatsTabAttrs>) {
    const histogramRows = attrs.data.allStats.filter((row) =>
      attrs.data.histograms.has(row.id),
    );

    return m(
      '.pf-trace-info-page__tab-content',
      m(
        Section,
        {
          title: 'Statistics',
          subtitle:
            'Complete dump of all trace statistics including errors, data losses, and debugging info',
        },
        m(StatsSection, {
          data: attrs.data.allStats,
          histograms: attrs.data.histograms,
          isMultiTrace: attrs.data.isMultiTrace,
          isMultiMachine: attrs.data.isMultiMachine,
        }),
      ),
      histogramRows.length > 0 &&
        m(
          Section,
          {
            title: 'Histograms',
            subtitle:
              'Bucket distributions for histogram-type trace statistics',
          },
          m(StatsHistogramsSection, {
            rows: histogramRows,
            histograms: attrs.data.histograms,
          }),
        ),
    );
  }
}

// Stats Section
interface StatsSectionAttrs {
  data: StatsRow[];
  histograms: Map<number, HistogramData>;
  isMultiTrace: boolean;
  isMultiMachine: boolean;
}

class StatsSection implements m.ClassComponent<StatsSectionAttrs> {
  private hideZeroValues = true;

  view({attrs}: m.CVnode<StatsSectionAttrs>) {
    const data = attrs.data;
    if (data === undefined || data.length === 0) {
      return m('');
    }

    const filtered = this.hideZeroValues
      ? data.filter((row) => row.value !== 0 && row.value !== null)
      : data;

    const columns = [
      ...(attrs.isMultiTrace
        ? [{key: 'trace', header: m(GridHeaderCell, 'Trace')}]
        : []),
      ...(attrs.isMultiMachine
        ? [{key: 'machine', header: m(GridHeaderCell, 'Machine')}]
        : []),
      {key: 'name', header: m(GridHeaderCell, 'Name')},
      {key: 'value', header: m(GridHeaderCell, 'Value')},
      {key: 'type', header: m(GridHeaderCell, 'Type')},
    ];

    const rowData = filtered.map((row) => {
      const idx = row.idx !== '' ? `[${row.idx}]` : '';
      const help = Boolean(row.description)
        ? m(
            Tooltip,
            {
              trigger: m(Icon, {
                icon: 'help_outline',
                className: 'pf-trace-info-page__help-icon',
              }),
            },
            `${row.description}`,
          )
        : undefined;
      const hasHistogram = attrs.histograms.has(row.id);
      const histogramLink = hasHistogram
        ? m(
            Tooltip,
            {
              trigger: m(Anchor, {
                icon: 'bar_chart',
                className: 'pf-trace-info-page__histogram-jump-link',
                onclick: () => scrollToStatHistogram(row.id),
              }),
            },
            'Jump to histogram',
          )
        : undefined;
      const cells = [];
      if (attrs.isMultiTrace) {
        cells.push(m(GridCell, row.traceId === null ? '-' : row.traceId));
      }
      if (attrs.isMultiMachine) {
        cells.push(m(GridCell, row.machineId === null ? '-' : row.machineId));
      }
      cells.push(
        m(
          GridCell,
          {className: 'pf-trace-info-page__grid-key'},
          `${row.name}${idx}`,
          help,
          histogramLink,
        ),
        m(GridCell, `${row.value}`),
        m(GridCell, `${row.severity} (${row.source})`),
      );
      return cells;
    });

    return m(
      'section.pf-trace-info-page__stats-section',
      m(Button, {
        label: this.hideZeroValues ? 'Show zero values' : 'Hide zero values',
        icon: this.hideZeroValues ? 'visibility' : 'visibility_off',
        onclick: () => {
          this.hideZeroValues = !this.hideZeroValues;
        },
      }),
      m(Grid, {
        columns,
        rowData,
        className: 'pf-trace-info-page__dense-grid',
      }),
    );
  }
}

interface StatsHistogramsSectionAttrs {
  rows: StatsRow[];
  histograms: Map<number, HistogramData>;
}

function histogramXAxisLabel(name: string): string {
  const withoutSuffix = name.replace(/_histogram$/, '');
  const match = /^(.*)_(ns|us|ms|s|bytes|kb|mb)$/.exec(withoutSuffix);
  if (match !== null) {
    return `${match[1]} (${match[2]})`;
  }
  return withoutSuffix;
}

class StatsHistogramsSection implements m.ClassComponent<StatsHistogramsSectionAttrs> {
  view({attrs}: m.CVnode<StatsHistogramsSectionAttrs>) {
    return m(
      Stack,
      {spacing: 'large'},
      attrs.rows.map((row) => {
        const histogram = attrs.histograms.get(row.id);
        if (histogram === undefined) return undefined;
        const idx = row.idx !== '' ? `[${row.idx}]` : '';
        const total = row.value ?? histogram.totalCount;
        const help = Boolean(row.description)
          ? m(
              Tooltip,
              {
                trigger: m(Icon, {
                  icon: 'help_outline',
                  className: 'pf-trace-info-page__help-icon',
                }),
              },
              `${row.description}`,
            )
          : undefined;

        return m(
          Card,
          {
            id: statHistogramElementId(row.id),
          },
          m(
            '.pf-trace-info-page__histogram-header',
            m('strong', `${row.name}${idx}`, help),
            m('span', `Total: ${total.toLocaleString()}`),
          ),
          m(HistogramSvg, {
            data: histogram,
            height: 200,
            xAxisLabel: histogramXAxisLabel(row.name),
            formatXValue: (v: number) =>
              Number.isFinite(v) ? formatNumber(v) : '∞',
          }),
        );
      }),
    );
  }
}
