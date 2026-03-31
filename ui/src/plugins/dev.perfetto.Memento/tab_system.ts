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
import {LineChart} from '../../components/widgets/charts/line_chart';
import {MementoSession, SnapshotData} from './memento_session';
import {buildSystemTimeSeries, computeT0} from './chart_builders';
import {formatKb, panel} from './utils';

interface SystemBillboards {
  totalKb: number;
  availableKb: number;
  anonKb: number;
  fileCacheKb: number;
  freeKb: number;
}

/** Latest system memory values for billboard display. */
function buildSystemBillboards(
  data: SnapshotData,
): SystemBillboards | undefined {
  const getLatest = (name: string): number => {
    const arr = data.systemCounters.get(name);
    if (arr === undefined || arr.length === 0) return 0;
    return Math.round(arr[arr.length - 1].value / 1024);
  };
  const totalKb = getLatest('MemTotal');
  if (totalKb === 0) return undefined;
  const anonKb = getLatest('Active(anon)') + getLatest('Inactive(anon)');
  const fileLruKb = getLatest('Active(file)') + getLatest('Inactive(file)');
  const shmemKb = getLatest('Shmem');
  return {
    totalKb,
    availableKb: getLatest('MemAvailable'),
    anonKb,
    fileCacheKb: Math.max(0, fileLruKb - shmemKb),
    freeKb: getLatest('MemFree'),
  };
}

export function renderSystemTab(session: MementoSession): m.Children {
  const data = session.data;
  if (!data) return null;

  const t0 = computeT0(data);
  const bb = buildSystemBillboards(data);
  const chartData = buildSystemTimeSeries(data, t0);

  return [
    bb !== undefined &&
      m(
        '.pf-memento-billboards',
        m(
          '.pf-memento-billboard',
          m('.pf-memento-billboard__value', formatKb(bb.totalKb)),
          m('.pf-memento-billboard__label', 'MemTotal'),
          m('.pf-memento-billboard__desc', 'Total physical RAM'),
        ),
        m(
          '.pf-memento-billboard',
          m('.pf-memento-billboard__value', formatKb(bb.availableKb)),
          m('.pf-memento-billboard__label', 'MemAvailable'),
          m('.pf-memento-billboard__desc', 'Available without swapping'),
        ),
        m(
          '.pf-memento-billboard',
          m('.pf-memento-billboard__value', formatKb(bb.anonKb)),
          m('.pf-memento-billboard__label', 'Anon'),
          m('.pf-memento-billboard__desc', 'Active(anon) + Inactive(anon)'),
        ),
        m(
          '.pf-memento-billboard',
          m('.pf-memento-billboard__value', formatKb(bb.fileCacheKb)),
          m('.pf-memento-billboard__label', 'Page Cache'),
          m('.pf-memento-billboard__desc', 'File LRU \u2212 Shmem'),
        ),
        m(
          '.pf-memento-billboard',
          m('.pf-memento-billboard__value', formatKb(bb.freeKb)),
          m('.pf-memento-billboard__label', 'MemFree'),
          m('.pf-memento-billboard__desc', 'Completely unused RAM'),
        ),
      ),

    panel(
      'System Memory',
      'Stacked areas partition MemTotal. Source: /proc/meminfo. ' +
        'Anon = Active(anon) + Inactive(anon). Page cache = Active(file) + Inactive(file) \u2212 Shmem. ' +
        'Unaccounted = MemTotal \u2212 sum of all other categories.',
      chartData
        ? m(LineChart, {
            data: chartData,
            height: 400,
            xAxisLabel: 'Time (s)',
            yAxisLabel: 'Memory',
            showLegend: true,
            showPoints: false,
            stacked: true,
            gridLines: 'horizontal',
            formatXValue: (v: number) => `${v.toFixed(0)}s`,
            formatYValue: (v: number) => formatKb(v),
          })
        : m('.pf-memento-placeholder', 'Waiting for data\u2026'),
    ),
  ];
}
