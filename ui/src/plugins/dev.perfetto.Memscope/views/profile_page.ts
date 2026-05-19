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

import './profile_page.scss';
import m from 'mithril';
import {
  LineChartSvg,
  type LineChartData,
} from '../../../components/widgets/charts_svg/line_chart_svg';
import {Button, ButtonVariant} from '../../../widgets/button';
import {Intent} from '../../../widgets/common';
import {Icon} from '../../../widgets/icon';
import {Billboard} from '../components/billboard';
import {ProgressBar} from '../components/progress_bar';
import {billboardKb, formatKb, maxSeriesKb, niceKbInterval} from '../utils';
import {Icons} from '../../../base/semantic_icons';
import {Stack} from '../../../widgets/stack';

export interface ProfilePageAttrs {
  readonly state: 'recording' | 'stopping' | 'finished';
  readonly bufferUsagePct?: number;
  readonly processName: string;
  readonly pid: number;
  readonly startMs: number;
  readonly chartData?: LineChartData;
  readonly baseline?: {anonSwap: number; file: number; dmabuf: number};
  readonly onStop: () => void;
  readonly onCancel: () => void;
}

function formatElapsed(startMs: number): string {
  const sec = Math.floor((Date.now() - startMs) / 1000);
  const mins = Math.floor(sec / 60);
  const secs = sec % 60;
  return mins > 0 ? `${mins}m ${secs}s` : `${secs}s`;
}

export class ProfilePage implements m.ClassComponent<ProfilePageAttrs> {
  view({attrs}: m.Vnode<ProfilePageAttrs>): m.Children {
    const {state, bufferUsagePct} = attrs;
    const stopped = state === 'stopping' || state === 'finished';
    return m(Stack, {spacing: 'large'}, [
      m(
        '.pf-memscope-profile-bar',
        {className: stopped ? 'pf-memscope-profile-bar--stopping' : undefined},
        [
          m(
            '.pf-memscope-profile-bar__zone.pf-memscope-profile-bar__zone--subject',
            [
              m(
                '.pf-memscope-profile-bar__label',
                stopped
                  ? m(
                      '.pf-memscope-profile-bar__stopping',
                      state === 'finished' ? 'Finalizing…' : 'Stopping…',
                    )
                  : m('.pf-memscope-profile-bar__rec-badge', 'Recording'),
              ),
              m(
                '.pf-memscope-profile-bar__field',
                m('.pf-memscope-profile-bar__process', attrs.processName),
                m('.pf-memscope-profile-bar__pid-chip', `PID ${attrs.pid}`),
                !stopped &&
                  m(
                    '.pf-memscope-profile-bar__duration',
                    formatElapsed(attrs.startMs),
                  ),
              ),
              !stopped &&
                m(ProgressBar, {
                  pct: bufferUsagePct ?? 0,
                  label: 'Ring buffer',
                  suffix: ' / 388 MB',
                }),
            ],
          ),

          !stopped &&
            m(
              '.pf-memscope-profile-bar__zone.pf-memscope-profile-bar__zone--sources',
              m('.pf-memscope-profile-bar__label', 'Recording config'),
              m(
                '.pf-memscope-profile-bar__field',
                m(
                  '.pf-memscope-source-chip',
                  m(
                    '.pf-memscope-source-chip__name',
                    m(Icon, {icon: 'memory'}),
                    'heapprofd',
                  ),
                ),
                m(
                  '.pf-memscope-source-chip',
                  m(
                    '.pf-memscope-source-chip__name',
                    m(Icon, {icon: 'local_cafe'}),
                    'java_hprof',
                    m('span.pf-memscope-source-chip__meta', 'every 10 s'),
                  ),
                ),
              ),
            ),

          !stopped &&
            m(
              '.pf-memscope-profile-bar__zone.pf-memscope-profile-bar__zone--actions',
              m(Button, {
                variant: ButtonVariant.Filled,
                label: 'Cancel',
                icon: 'close',
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
        ],
      ),

      renderBillboards(attrs.chartData, attrs.baseline),
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

function xRange(data: LineChartData): {
  xMin: number | undefined;
  xMax: number | undefined;
} {
  let xMin: number | undefined;
  let xMax: number | undefined;
  for (const s of data.series) {
    for (const p of s.points) {
      if (xMin === undefined || p.x < xMin) xMin = p.x;
      if (xMax === undefined || p.x > xMax) xMax = p.x;
    }
  }
  return {xMin, xMax};
}

function renderBreakdownChart(attrs: ProfilePageAttrs): m.Children {
  const {chartData} = attrs;
  const body = chartData
    ? (() => {
        const {xMin, xMax} = xRange(chartData);
        return m(LineChartSvg, {
          data: {series: chartData.series},
          height: 350,
          xAxisLabel: 'Time (s)',
          yAxisLabel: 'Memory',
          showLegend: true,
          showPoints: false,
          stacked: true,
          gridLines: 'both',
          xAxisMin: xMin,
          xAxisMax: xMax,
          formatXValue: (v: number) => `${v.toFixed(0)}s`,
          formatYValue: (v: number) => formatKb(v),
          yAxisMinInterval: niceKbInterval(maxSeriesKb(chartData.series)),
        });
      })()
    : m('.pf-memscope-placeholder', 'Waiting for data…');

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
    m('.pf-memscope-panel__body', body),
  );
}
