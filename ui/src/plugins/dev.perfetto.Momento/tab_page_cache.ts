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
import {
  LineChart,
  LineChartData,
} from '../../components/widgets/charts/line_chart';
import {formatKb, panel} from './utils';

export interface PageCacheTabData {
  pageCacheChartData?: LineChartData;
  fileCacheBreakdownData?: LineChartData;
  fileCacheActivityData?: LineChartData;
  xAxisMin?: number;
  xAxisMax?: number;
}

export function getPageCacheBillboards(
  fileCacheBreakdownData?: LineChartData,
): {total: number; dirty: number; mapped: number} | undefined {
  const data = fileCacheBreakdownData;
  if (data === undefined || data.series.length < 4) return undefined;
  // Series order: Mapped+Dirty, Mapped+Clean, Unmapped+Dirty, Unmapped+Clean
  const last = (idx: number) => {
    const pts = data.series[idx].points;
    return pts.length > 0 ? pts[pts.length - 1].y : 0;
  };
  const mappedDirty = last(0);
  const mappedClean = last(1);
  const unmappedDirty = last(2);
  const unmappedClean = last(3);
  return {
    total: mappedDirty + mappedClean + unmappedDirty + unmappedClean,
    dirty: mappedDirty + unmappedDirty,
    mapped: mappedDirty + mappedClean,
  };
}

export function renderPageCacheTab(data: PageCacheTabData): m.Children {
  const billboards = getPageCacheBillboards(data.fileCacheBreakdownData);

  return [
    billboards !== undefined &&
      m(
        '.pf-live-memory-billboards',
        m(
          '.pf-live-memory-billboard',
          m('.pf-live-memory-billboard__value', formatKb(billboards.total)),
          m('.pf-live-memory-billboard__label', 'Total Page Cache'),
          m(
            '.pf-live-memory-billboard__desc',
            'Derived: Active(file) + Inactive(file) from /proc/meminfo',
          ),
        ),
        m(
          '.pf-live-memory-billboard',
          m('.pf-live-memory-billboard__value', formatKb(billboards.dirty)),
          m('.pf-live-memory-billboard__label', 'Dirty'),
          m(
            '.pf-live-memory-billboard__desc',
            'Source: Dirty from /proc/meminfo',
          ),
        ),
        m(
          '.pf-live-memory-billboard',
          m('.pf-live-memory-billboard__value', formatKb(billboards.mapped)),
          m('.pf-live-memory-billboard__label', 'Mapped'),
          m(
            '.pf-live-memory-billboard__desc',
            'Source: Mapped from /proc/meminfo',
          ),
        ),
      ),

    panel(
      'Page Cache',
      'Source: /proc/meminfo counters Active(file), Inactive(file), Shmem. ' +
        'Stacked: Active(file) + Inactive(file) + Shmem \u2248 Cached.',
      data.pageCacheChartData
        ? m(LineChart, {
            data: data.pageCacheChartData,
            height: 250,
            xAxisLabel: 'Time (s)',
            yAxisLabel: 'Cache',
            showLegend: true,
            showPoints: false,
            stacked: true,
            gridLines: 'horizontal',
            formatXValue: (v: number) => `${v.toFixed(0)}s`,
            formatYValue: (v: number) => formatKb(v),
            xAxisMin: data.xAxisMin,
            xAxisMax: data.xAxisMax,
          })
        : m('.pf-live-memory-placeholder', 'Waiting for data\u2026'),
    ),

    panel(
      'Page Cache Activity',
      'Source: /proc/vmstat counters, shown as rates (delta/s). ' +
        'Refaults = workingset_refault_file (evicted pages needed again). ' +
        'Stolen = pgsteal_file (pages reclaimed). ' +
        'Scanned = pgscan_file (pages considered for reclaim).',
      data.fileCacheActivityData
        ? m(LineChart, {
            data: data.fileCacheActivityData,
            height: 200,
            xAxisLabel: 'Time (s)',
            yAxisLabel: 'Pages/s',
            showLegend: true,
            showPoints: false,
            gridLines: 'horizontal',
            formatXValue: (v: number) => `${v.toFixed(0)}s`,
            formatYValue: (v: number) => v.toLocaleString(),
            xAxisMin: data.xAxisMin,
            xAxisMax: data.xAxisMax,
          })
        : m('.pf-live-memory-placeholder', 'Waiting for data\u2026'),
    ),
  ];
}
