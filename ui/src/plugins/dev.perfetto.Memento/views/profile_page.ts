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
} from '../../../components/widgets/charts/line_chart';
import {Button, ButtonVariant} from '../../../widgets/button';
import {Chip} from '../../../widgets/chip';
import {Icon} from '../../../widgets/icon';
import {Intent} from '../../../widgets/common';
import {Billboard} from '../components/billboard';
import {billboardKb, formatKb, maxSeriesKb, niceKbInterval} from '../utils';
import {Icons} from '../../../base/semantic_icons';
import {BillboardRow} from '../components/billboard_row';
import {ProcessProfileSession} from '../sessions/process_profile_session';

export interface ProfilePageAttrs {
  readonly session: ProcessProfileSession;
  processName: string;
  pid: number;
  stopping: boolean;
  duration: string;
  chartData?: LineChartData;
  baseline?: {anonSwap: number; file: number; dmabuf: number};
  xMin: number;
  xMax: number;
  /** x-axis position (s relative to ts0) where profiling started. */
  startX?: number;
  onStop: () => void;
  onCancel: () => void;
}

export class ProfilePage
  implements m.ClassComponent<ProfilePageAttrs>
{
  view({attrs}: m.Vnode<ProfilePageAttrs>): m.Children {
    const {session} = attrs;
    return [
      // Profile header bar with status indicators.
      m(
        '.pf-memento-profile-bar',
        // Left: cancel button + process identity.
        m(
          '.pf-memento-profile-bar__left',
          !attrs.stopping &&
            m(Button, {
              label: 'Cancel',
              icon: 'arrow_back',
              variant: ButtonVariant.Filled,
              onclick: () => attrs.onCancel(),
            }),
          m(Icon, {icon: 'science'}),
          m(
            '.pf-memento-profile-bar__name',
            `Profiling: ${attrs.processName} (PID ${attrs.pid})`,
            attrs.stopping && m(Chip, {label: 'Stopping\u2026'}),
          ),
        ),
        // Center: what is being recorded (big).
        !attrs.stopping &&
          m(
            '.pf-memento-profile-bar__recording',
            m(
              '.pf-memento-profile-bar__datasource',
              m(Icon, {icon: 'memory'}),
              m('span', 'heapprofd'),
              m('span.pf-memento-profiling-status__active', 'recording'),
            ),
            m(
              '.pf-memento-profile-bar__datasource',
              m(Icon, {icon: 'coffee'}),
              m('span', 'java_hprof'),
              m('span.pf-memento-profiling-status__active', 'recording'),
            ),
          ),
        // Stats: elapsed time + buffer usage.
        m(
          '.pf-memento-profile-bar__stats',
          attrs.duration && m(Chip, {label: attrs.duration}),
          !attrs.stopping &&
            session.bufferUsagePct !== undefined &&
            m(
              '.pf-memento-profile-bar__buffer-stat',
              m(Icon, {icon: 'storage'}),
              `Buffer ${session.bufferUsagePct.toFixed(1)}%`,
            ),
        ),
        // Right: stop action.
        m(
          '.pf-memento-profile-bar__actions',
          !attrs.stopping &&
            m(Button, {
              label: 'Stop & Open Trace',
              icon: Icons.ExternalLink,
              variant: ButtonVariant.Filled,
              intent: Intent.Primary,
              onclick: () => attrs.onStop(),
            }),
        ),
      ),

      // Billboards.
      renderBillboards(attrs.chartData, attrs.baseline),

      // Process memory breakdown chart.
      renderBreakdownChart(attrs),
    ];
  }
}

function renderBillboards(
  chartData?: LineChartData,
  baseline?: {anonSwap: number; file: number; dmabuf: number},
): m.Children {
  if (chartData === undefined || chartData.series.length === 0) return null;

  const seriesColor = (name: string): string | undefined =>
    chartData.series.find((sr) => sr.name === name)?.color;

  const latest = (name: string): number => {
    const s = chartData.series.find((sr) => sr.name === name);
    if (s === undefined || s.points.length === 0) return 0;
    return s.points[s.points.length - 1].y;
  };

  const card = (
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
    return m(Billboard, {
      ...billboardKb(current),
      label,
      desc,
      delta: deltaStr,
      color: seriesColor(label),
    });
  };

  return m(BillboardRow, [
    card(
      latest('Anon + Swap'),
      baseline?.anonSwap,
      'Anon + Swap',
      'Anonymous resident + swapped pages',
    ),
    card(latest('File'), baseline?.file, 'File', 'File-backed resident pages'),
    card(latest('DMA-BUF'), baseline?.dmabuf, 'DMA-BUF', 'GPU/DMA buffer RSS'),
  ]);
}

function renderBreakdownChart(attrs: ProfilePageAttrs): m.Children {
  return m(
    '.pf-memento-panel',
    m(
      '.pf-memento-panel__header',
      m('h2', 'Process Memory Breakdown'),
      m(
        'p',
        `Stacked area chart of memory usage for ${attrs.processName}. ` +
          'Anon + Swap = anonymous resident + swapped pages. ' +
          'File = file-backed resident pages. DMA-BUF = GPU/DMA buffer RSS.',
      ),
    ),
    attrs.chartData
      ? m(LineChart, {
          data: {
            series: attrs.chartData.series,
            markers:
              attrs.startX !== undefined
                ? [{x: attrs.startX, label: 'Profile start', color: '#2196f3'}]
                : undefined,
          },
          height: 350,
          xAxisLabel: 'Time (s)',
          yAxisLabel: 'Memory',
          showLegend: true,
          showPoints: false,
          stacked: true,
          gridLines: 'both',
          xAxisMin: attrs.xMin,
          xAxisMax: attrs.xMax,
          formatXValue: (v: number) => `${v.toFixed(0)}s`,
          formatYValue: (v: number) => formatKb(v),
          yAxisMinInterval: niceKbInterval(maxSeriesKb(attrs.chartData.series)),
        })
      : m('.pf-memento-placeholder', 'Waiting for data\u2026'),
  );
}
