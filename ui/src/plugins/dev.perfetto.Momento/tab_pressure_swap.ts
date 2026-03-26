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

interface LmkEvent {
  ts: number;
  pid: number;
  processName: string;
  oomScoreAdj: number;
}

export interface PressureSwapTabData {
  psiChartData?: LineChartData;
  pageFaultChartData?: LineChartData;
  swapChartData?: LineChartData;
  vmstatChartData?: LineChartData;
  lmkEvents: LmkEvent[];
  traceT0: number;
  xAxisMin?: number;
  xAxisMax?: number;
}

function renderLmkPanel(events: LmkEvent[], t0: number): m.Children {
  if (events.length === 0) return null;
  return panel(
    `LMK Kills (${events.length})`,
    'Low Memory Killer events recorded during this session. ' +
      'Source: lmkd atrace / lowmemorykiller ftrace.',
    m(
      'table.pf-live-memory-table',
      m(
        'tr',
        m('th', 'Time'),
        m('th', 'PID'),
        m('th', 'Process'),
        m('th', 'OOM Adj'),
      ),
      events.map((ev) =>
        m(
          'tr',
          m('td', `${((ev.ts - t0) / 1e9).toFixed(1)}s`),
          m('td', String(ev.pid)),
          m('td', ev.processName || '(unknown)'),
          m('td', String(ev.oomScoreAdj)),
        ),
      ),
    ),
  );
}

export function renderPressureSwapTab(data: PressureSwapTabData): m.Children {
  return [
    renderLmkPanel(data.lmkEvents, data.traceT0),

    panel(
      'Memory Pressure (PSI)',
      'Source: /proc/pressure/memory (psi.mem.some, psi.mem.full). ' +
        'Derived: cumulative \u00b5s converted to ms/s rate. ' +
        '"some" = at least one task stalled, "full" = all tasks stalled.',
      data.psiChartData
        ? m(LineChart, {
            data: data.psiChartData,
            height: 200,
            xAxisLabel: 'Time (s)',
            yAxisLabel: 'Stall (ms/s)',
            showLegend: true,
            showPoints: false,
            gridLines: 'horizontal',
            formatXValue: (v: number) => `${v.toFixed(0)}s`,
            formatYValue: (v: number) => `${v.toFixed(1)} ms/s`,
            xAxisMin: data.xAxisMin,
            xAxisMax: data.xAxisMax,
          })
        : m('.pf-live-memory-placeholder', 'Waiting for data\u2026'),
    ),

    panel(
      'Page Faults',
      'Source: /proc/vmstat counters pgfault, pgmajfault. ' +
        'Derived: cumulative counts converted to faults/s rate. ' +
        'Minor (pgfault) = page in RAM but not in TLB. Major (pgmajfault) = page must be read from disk.',
      data.pageFaultChartData
        ? m(LineChart, {
            data: data.pageFaultChartData,
            height: 200,
            xAxisLabel: 'Time (s)',
            yAxisLabel: 'Faults/s',
            showLegend: true,
            showPoints: false,
            gridLines: 'horizontal',
            formatXValue: (v: number) => `${v.toFixed(0)}s`,
            formatYValue: (v: number) => `${v.toFixed(0)} f/s`,
            xAxisMin: data.xAxisMin,
            xAxisMax: data.xAxisMax,
          })
        : m('.pf-live-memory-placeholder', 'Waiting for data\u2026'),
    ),

    data.swapChartData &&
      panel(
        'Swap Usage',
        'Source: /proc/meminfo counters SwapTotal, SwapFree, SwapCached. ' +
          'Derived: Swap dirty = (SwapTotal \u2212 SwapFree) \u2212 SwapCached.',
        m(LineChart, {
          data: data.swapChartData,
          height: 200,
          xAxisLabel: 'Time (s)',
          yAxisLabel: 'Swap',
          showLegend: true,
          showPoints: false,
          stacked: true,
          gridLines: 'horizontal',
          formatXValue: (v: number) => `${v.toFixed(0)}s`,
          formatYValue: (v: number) => formatKb(v),
          xAxisMin: data.xAxisMin,
          xAxisMax: data.xAxisMax,
        }),
      ),

    data.vmstatChartData &&
      panel(
        'Swap I/O (pswpin / pswpout)',
        'Source: /proc/vmstat counters pswpin, pswpout. ' +
          'Derived: cumulative page counts converted to pages/s rate.',
        m(LineChart, {
          data: data.vmstatChartData,
          height: 200,
          xAxisLabel: 'Time (s)',
          yAxisLabel: 'Pages/s',
          showLegend: true,
          showPoints: false,
          gridLines: 'horizontal',
          formatXValue: (v: number) => `${v.toFixed(0)}s`,
          formatYValue: (v: number) => `${v.toFixed(0)} pg/s`,
          xAxisMin: data.xAxisMin,
          xAxisMax: data.xAxisMax,
        }),
      ),
  ];
}
