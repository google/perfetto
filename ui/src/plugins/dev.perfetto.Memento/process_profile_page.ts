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
import {Button, ButtonVariant} from '../../widgets/button';
import {Chip} from '../../widgets/chip';
import {Icon} from '../../widgets/icon';
import {Intent} from '../../widgets/common';
import {billboardKb, formatKb} from './utils';
import {Icons} from '../../base/semantic_icons';

export interface ProfilePageData {
  processName: string;
  pid: number;
  stopping: boolean;
  duration: string;
  chartData?: LineChartData;
  baseline?: {anonSwap: number; file: number; dmabuf: number};
  xMin: number;
  xMax: number;
}

export interface ProfilePageCallbacks {
  onStop: () => void;
  onCancel: () => void;
}

export function renderProcessProfilePage(
  data: ProfilePageData,
  callbacks: ProfilePageCallbacks,
): m.Children {
  return [
    // Profile header bar with status indicators.
    m(
      '.pf-memento-profile-bar',
      m(
        '.pf-memento-profile-bar__left',
        !data.stopping &&
          m(Button, {
            label: 'Cancel',
            icon: 'arrow_back',
            variant: ButtonVariant.Filled,
            onclick: () => callbacks.onCancel(),
          }),
        m(Icon, {icon: 'science'}),
        m(
          '.pf-memento-profile-bar__title',
          m(
            '.pf-memento-profile-bar__name',
            `Profiling: ${data.processName} (PID ${data.pid})`,
            data.duration && m(Chip, {label: data.duration}),
            data.stopping && m(Chip, {label: 'Stopping\u2026'}),
          ),
          !data.stopping &&
            m(
              '.pf-memento-profile-bar__datasources',
              m(Icon, {icon: 'memory'}),
              'heapprofd ',
              m('span.pf-memento-profiling-status__active', 'recording'),
              '\u00b7',
              m(Icon, {icon: 'coffee'}),
              'java_hprof ',
              m('span.pf-memento-profiling-status__active', 'recording'),
            ),
        ),
      ),
      m(
        '.pf-memento-profile-bar__actions',
        !data.stopping &&
          m(Button, {
            label: 'Stop & Open Trace',
            icon: Icons.ExternalLink,
            variant: ButtonVariant.Filled,
            intent: Intent.Primary,
            onclick: () => callbacks.onStop(),
          }),
      ),
    ),

    // Billboards.
    renderBillboards(data.chartData, data.baseline),

    // Process memory breakdown chart.
    renderBreakdownChart(data),
  ];
}

function renderBillboards(
  chartData?: LineChartData,
  baseline?: {anonSwap: number; file: number; dmabuf: number},
): m.Children {
  if (chartData === undefined || chartData.series.length === 0) return null;

  const latest = (name: string): number => {
    const s = chartData.series.find((sr) => sr.name === name);
    if (s === undefined || s.points.length === 0) return 0;
    return s.points[s.points.length - 1].y;
  };

  const billboard = (
    current: number,
    baselineVal: number | undefined,
    label: string,
    desc: string,
  ) => {
    const delta = baselineVal !== undefined ? current - baselineVal : undefined;
    const deltaStr =
      delta !== undefined
        ? `${delta >= 0 ? '+' : ''}${formatKb(delta)}`
        : undefined;
    return m(
      '.pf-memento-billboard',
      m('.pf-memento-billboard__value', billboardKb(current)),
      deltaStr !== undefined &&
        m(
          '.pf-memento-billboard__delta',
          {
            class:
              delta! > 0
                ? 'pf-memento-billboard__delta--up'
                : delta! < 0
                  ? 'pf-memento-billboard__delta--down'
                  : '',
          },
          deltaStr,
        ),
      m('.pf-memento-billboard__label', label),
      m('.pf-memento-billboard__desc', desc),
    );
  };

  return m(
    '.pf-memento-billboards',
    billboard(
      latest('Anon + Swap'),
      baseline?.anonSwap,
      'Anon + Swap',
      'Anonymous resident + swapped pages',
    ),
    billboard(
      latest('File'),
      baseline?.file,
      'File',
      'File-backed resident pages',
    ),
    billboard(
      latest('DMA-BUF'),
      baseline?.dmabuf,
      'DMA-BUF',
      'GPU/DMA buffer RSS',
    ),
  );
}

function renderBreakdownChart(data: ProfilePageData): m.Children {
  return m(
    '.pf-memento-panel',
    m(
      '.pf-memento-panel__header',
      m('h2', 'Process Memory Breakdown'),
      m(
        'p',
        `Stacked area chart of memory usage for ${data.processName}. ` +
          'Anon + Swap = anonymous resident + swapped pages. ' +
          'File = file-backed resident pages. DMA-BUF = GPU/DMA buffer RSS.',
      ),
    ),
    data.chartData
      ? m(LineChart, {
          data: data.chartData,
          height: 350,
          xAxisLabel: 'Time (s)',
          yAxisLabel: 'Memory',
          showLegend: true,
          showPoints: false,
          stacked: true,
          gridLines: 'horizontal',
          xAxisMin: data.xMin,
          xAxisMax: data.xMax,
          formatXValue: (v: number) => `${v.toFixed(0)}s`,
          formatYValue: (v: number) => formatKb(v),
        })
      : m('.pf-memento-placeholder', 'Waiting for data\u2026'),
  );
}
