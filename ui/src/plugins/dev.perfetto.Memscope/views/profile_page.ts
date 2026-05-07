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
  LineChartSvg,
  LineChartData,
} from '../../../components/widgets/charts_svg/line_chart_svg';
import {Button, ButtonVariant} from '../../../widgets/button';
import {Icon} from '../../../widgets/icon';
import {Intent} from '../../../widgets/common';
import {Billboard} from '../components/billboard';
import {billboardKb, formatKb, maxSeriesKb, niceKbInterval} from '../utils';
import {Icons} from '../../../base/semantic_icons';
import {Stack} from '../../../widgets/stack';
import {ProfileSession} from '../sessions/profile_session';

export interface ProfilePageAttrs {
  readonly session: ProfileSession;
  processName: string;
  pid: number;
  duration: string;
  chartData?: LineChartData;
  baseline?: {anonSwap: number; file: number; dmabuf: number};
  /** x-axis position (s relative to ts0) where profiling started. */
  startX?: number;
  onStop: () => void;
  onCancel: () => void;
}

export class ProfilePage implements m.ClassComponent<ProfilePageAttrs> {
  view({attrs}: m.Vnode<ProfilePageAttrs>): m.Children {
    const {session} = attrs;
    const isStopping =
      session.state === 'stopping' || session.state === 'finished';
    return m(Stack, {spacing: 'large'}, [
      // Profile header bar with status indicators.
      m(
        '.pf-memscope-profile-bar',

        // Zone 1 — "We are recording right now".
        m(
          '.pf-memscope-profile-bar__status',
          isStopping
            ? m(
                '.pf-memscope-profile-bar__stopping',
                session.state === 'finished'
                  ? 'Finalizing\u2026'
                  : 'Stopping\u2026',
              )
            : m('.pf-memscope-profile-bar__rec-badge', 'Profiling'),
          !isStopping &&
            m(
              '.pf-memscope-profile-bar__sources',
              m(
                '.pf-memscope-source-chip',
                m(
                  '.pf-memscope-source-chip__name',
                  m(Icon, {icon: 'memory'}),
                  'heapprofd',
                ),
                m(
                  '.pf-memscope-source-chip__stats',
                  m('span', '4\u202fKB sample'),
                  m('span.pf-memscope-source-chip__stat-sep', '\u00b7'),
                  m('span', 'all heaps'),
                  m('span.pf-memscope-source-chip__stat-sep', '\u00b7'),
                  m('span', '128\u202fMB buf'),
                ),
              ),
              m('span.pf-memscope-profile-bar__source-sep', '\u00b7'),
              m(
                '.pf-memscope-source-chip',
                m(
                  '.pf-memscope-source-chip__name',
                  m(Icon, {icon: 'coffee'}),
                  'java_hprof',
                ),
                m(
                  '.pf-memscope-source-chip__stats',
                  m('span', '10\u202fs dumps'),
                  m('span.pf-memscope-source-chip__stat-sep', '\u00b7'),
                  m('span', '512\u202fMB buf'),
                ),
              ),
            ),
        ),

        // Zone 2 — Stats: what process, how long, buffer fill.
        m(
          '.pf-memscope-profile-bar__subject',
          m('.pf-memscope-profile-bar__process', attrs.processName),
          m(
            '.pf-memscope-profile-bar__meta',
            m(Icon, {icon: 'tag'}),
            `PID\u00a0${attrs.pid}`,
            attrs.duration && [
              m('span.pf-memscope-profile-bar__meta-sep', '\u00b7'),
              m(Icon, {icon: 'schedule'}),
              attrs.duration,
            ],
            !isStopping &&
              session.bufferUsagePct !== undefined && [
                m('span.pf-memscope-profile-bar__meta-sep', '\u00b7'),
                m(Icon, {icon: 'storage'}),
                `Buffer\u00a0${session.bufferUsagePct.toFixed(1)}%`,
              ],
          ),
        ),

        // Zone 3 — Actions: cancel or stop.
        !isStopping &&
          m(
            '.pf-memscope-profile-bar__actions',
            m(Button, {
              label: 'Cancel',
              icon: 'arrow_back',
              onclick: () => attrs.onCancel(),
            }),
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
    ]);
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

  return m(Stack, {orientation: 'horizontal', spacing: 'large'}, [
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
    '.pf-memscope-panel',
    m(
      '.pf-memscope-panel__header',
      m('h2', 'Process Memory Breakdown'),
      m(
        'p',
        `Stacked area chart of memory usage for ${attrs.processName}. ` +
          'Anon + Swap = anonymous resident + swapped pages. ' +
          'File = file-backed resident pages. DMA-BUF = GPU/DMA buffer RSS.',
      ),
    ),
    attrs.chartData
      ? m(LineChartSvg, {
          data: {
            series: attrs.chartData.series,
          },
          height: 350,
          xAxisLabel: 'Time (s)',
          yAxisLabel: 'Memory',
          showLegend: true,
          showPoints: false,
          stacked: true,
          gridLines: 'both',
          xAxisMin: attrs.startX,
          formatXValue: (v: number) =>
            `${(v - (attrs.startX ?? 0)).toFixed(0)}s`,
          formatYValue: (v: number) => formatKb(v),
          yAxisMinInterval: niceKbInterval(maxSeriesKb(attrs.chartData.series)),
        })
      : m('.pf-memscope-placeholder', 'Waiting for data\u2026'),
  );
}
