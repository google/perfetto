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
import {Sankey, SankeyData} from '../../components/widgets/charts/sankey';
import {formatKb, panel} from './utils';

export interface SystemTabData {
  systemChartData?: LineChartData;
  sankeyData?: SankeyData;
  xAxisMin?: number;
  xAxisMax?: number;
}

export function renderSystemTab(data: SystemTabData): m.Children {
  return [
    panel(
      'System Memory Overview',
      'Physical RAM breakdown by category. Source: /proc/meminfo. Unaccounted = MemTotal minus all named categories.',
      data.sankeyData
        ? m(Sankey, {
            data: data.sankeyData,
            height: 350,
            formatValue: (v: number) => formatKb(v),
          })
        : m('.pf-live-memory-placeholder', 'Waiting for data\u2026'),
    ),

    panel(
      'System Memory',
      'Stacked areas partition MemTotal. Source: /proc/meminfo. ' +
        'Anon = Active(anon) + Inactive(anon). Page cache = Active(file) + Inactive(file) \u2212 Shmem. ' +
        'Unaccounted = MemTotal \u2212 sum of all other categories.',
      data.systemChartData
        ? m(LineChart, {
            data: data.systemChartData,
            height: 400,
            xAxisLabel: 'Time (s)',
            yAxisLabel: 'Memory',
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
  ];
}
