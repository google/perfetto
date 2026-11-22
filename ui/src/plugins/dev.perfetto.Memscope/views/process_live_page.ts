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
import {formatBytesIec} from '../../../base/bytes_format';
import {GateDetector} from '../../../base/mithril_utils';
import {
  LiveStripChart,
  type StripSeries,
} from '../../../components/widgets/charts/live_strip_chart';
import type {
  LineChartData,
  LineChartSeries,
} from '../../../components/widgets/charts_svg/line_chart_svg';
import {Button, ButtonVariant} from '../../../widgets/button';
import {Intent} from '../../../widgets/common';
import {Billboard, BillboardStrip} from '../components/billboard';
import {Page} from '../components/page';
import {Panel} from '../components/panel';
import {PreviewBanner} from '../components/preview_banner';
import {ProcessHeader, type ProcessOption} from '../components/process_header';
import {matchProcessesByName} from '../process_focus';
import type {ProcessInfo, SnapshotData} from '../sessions/live_session';
import {billboardBytes, counterPoints} from '../utils';
import {
  buildProcessMemoryBreakdown,
  LiveProfileView,
  type LiveSessionAttrs,
  SessionPill,
} from './live_chrome';
import {ProcessesTab} from './processes_table';
import {
  renderSystemBillboards,
  renderSystemChart,
  renderSystemSummary,
} from './system_summary';

interface ProcessRow {
  readonly info: ProcessInfo;
  readonly rssBytes: number;
}

function latest(data: SnapshotData, upid: number, counter: string): number {
  const samples = data.processCountersByUpid.get(upid)?.get(counter);
  if (samples === undefined || samples.length === 0) return 0;
  return samples[samples.length - 1]!.value;
}

function processRows(data: SnapshotData): ProcessRow[] {
  const rows: ProcessRow[] = [];
  for (const info of data.processInfo.values()) {
    rows.push({info, rssBytes: latest(data, info.upid, 'mem.rss')});
  }
  rows.sort((a, b) => b.rssBytes - a.rssBytes);
  return rows;
}

const COUNTER_SERIES: ReadonlyArray<{
  readonly counter: string;
  readonly name: string;
  readonly color: string;
}> = [
  {counter: 'mem.rss.anon', name: 'RSS Anon', color: '#7c3aed'},
  {counter: 'mem.rss.file', name: 'RSS File', color: '#16a34a'},
  {counter: 'mem.rss.shmem', name: 'RSS Shmem', color: '#2563eb'},
  {counter: 'mem.swap', name: 'Swap', color: '#f59e0b'},
  {counter: 'mem.dmabuf_rss', name: 'DMA-BUF', color: '#ec4899'},
];

function buildCounterChart(
  data: SnapshotData,
  upid: number,
): LineChartData | undefined {
  const counters = data.processCountersByUpid.get(upid);
  if (counters === undefined) return undefined;
  const series: LineChartSeries[] = [];
  for (const {counter, name, color} of COUNTER_SERIES) {
    const pts = counterPoints(counters.get(counter), data.ts0);
    if (pts === undefined || pts.length < 2) continue;
    series.push({
      name,
      color,
      points: pts.map((p) => ({x: p.x, y: p.y / 1024})),
    });
  }
  return series.length > 0 ? {series} : undefined;
}

// Device context is secondary to the process, so its chart is shorter.
const SYSTEM_CHART_HEIGHT = 220;

export interface ProcessLivePageAttrs extends LiveSessionAttrs {
  // The process named by the route's `process` arg, if any.
  readonly focusProcess?: string;
  readonly onFocus: (processName: string) => void;
  readonly onClearFocus: () => void;
}

// Live view of a single process: pick one from the device's running
// processes, then watch its memory and capture heap profiles of it.
export class ProcessLivePage implements m.ClassComponent<ProcessLivePageAttrs> {
  // Disambiguates between processes sharing the focused name.
  private preferredUpid?: number;

  view({attrs}: m.CVnode<ProcessLivePageAttrs>) {
    const {session} = attrs;
    const proc = this.resolve(attrs);
    return m(
      GateDetector,
      {
        onVisibilityChanged: (visible) =>
          visible ? session.resume() : session.pause(),
      },
      m(
        Page,
        this.renderHeader(attrs, proc),
        m(PreviewBanner, {app: attrs.app}),
        session.profile !== undefined
          ? m(LiveProfileView, attrs)
          : this.renderBody(attrs, proc),
      ),
    );
  }

  private resolve(attrs: ProcessLivePageAttrs): ProcessInfo | undefined {
    const data = attrs.session.data;
    const name = attrs.focusProcess;
    if (data === undefined || name === undefined) return undefined;
    const preferred =
      this.preferredUpid === undefined
        ? undefined
        : data.processInfo.get(this.preferredUpid);
    if (preferred?.processName === name) return preferred;
    const rows = processRows(data);
    const matches = matchProcessesByName(rows, name, (r) => r.info.processName);
    // processRows() is sorted by RSS, so the first match is the biggest.
    return matches[0]?.info;
  }

  private select(attrs: ProcessLivePageAttrs, info: ProcessInfo) {
    this.preferredUpid = info.upid;
    attrs.onFocus(info.processName);
  }

  private clearFocus(attrs: ProcessLivePageAttrs) {
    this.preferredUpid = undefined;
    attrs.onClearFocus();
  }

  private renderHeader(
    attrs: ProcessLivePageAttrs,
    proc: ProcessInfo | undefined,
  ): m.Children {
    const {session, focusProcess} = attrs;
    const data = session.data;
    const rows = data === undefined ? [] : processRows(data);
    const options: ProcessOption[] = rows.map((r) => ({
      key: `${r.info.upid}`,
      name: r.info.processName,
      pid: r.info.pid,
      detail: formatBytesIec(r.rssBytes),
    }));
    const waiting =
      focusProcess !== undefined && data !== undefined && proc === undefined;
    return m(ProcessHeader, {
      title: 'Memscope',
      options,
      selectedKey: proc === undefined ? undefined : `${proc.upid}`,
      onClear: () => this.clearFocus(attrs),
      canClear: proc !== undefined || focusProcess !== undefined,
      clearLabel: 'All processes',
      unselectedLabel: 'All processes',
      disabled: session.profile !== undefined,
      onSelect: (o) => {
        const info = data?.processInfo.get(Number(o.key));
        if (info !== undefined) this.select(attrs, info);
      },
      note: waiting ? `Waiting for ${focusProcess} to start…` : undefined,
      trailing: m(SessionPill, attrs),
    });
  }

  private renderBody(
    attrs: ProcessLivePageAttrs,
    proc: ProcessInfo | undefined,
  ): m.Children {
    const data = attrs.session.data;
    if (data === undefined) {
      return m('.pf-memscope-placeholder', 'Waiting for data…');
    }
    if (proc === undefined) {
      return this.renderPicker(attrs, data);
    }
    return this.renderProcess(attrs, data, proc);
  }

  private renderPicker(
    attrs: ProcessLivePageAttrs,
    data: SnapshotData,
  ): m.Children {
    return [
      renderSystemBillboards(data),
      m(ProcessesTab, {
        app: attrs.app,
        data,
        session: attrs.session,
        companion: renderSystemChart(data, true, 350, !attrs.session.isPaused),
        rowActions: (p) =>
          m(Button, {
            label: 'Open',
            rightIcon: 'arrow_forward',
            variant: ButtonVariant.Filled,
            intent: Intent.Primary,
            rounded: true,
            onclick: () => {
              const info = [...data.processInfo.values()].find(
                (i) => i.pid === p.pid,
              );
              if (info !== undefined) this.select(attrs, info);
            },
          }),
      }),
    ];
  }

  private renderProcess(
    attrs: LiveSessionAttrs,
    data: SnapshotData,
    proc: ProcessInfo,
  ): m.Children {
    const {session} = attrs;
    const upid = proc.upid;
    const baseSec = data.ts0 / 1e9;
    const latestSec = baseSec + data.xMax;
    const isPlaying = !session.isPaused;
    const chart = (chartData: LineChartData | undefined, stacked: boolean) => {
      if (!chartData) {
        return m('.pf-memscope-placeholder', 'Waiting for data…');
      }
      let maxPointT = -Infinity;
      const stripSeries: StripSeries[] = chartData.series
        .filter((s) => s.points.length > 0)
        .map((s) => ({
          name: s.name,
          color: s.color ?? '#2563eb',
          points: s.points.map((p) => {
            const t = baseSec + p.x;
            if (t > maxPointT) maxPointT = t;
            return {t, v: p.y};
          }),
        }));
      if (stripSeries.length === 0) {
        return m('.pf-memscope-placeholder', 'Waiting for data…');
      }
      return m(LiveStripChart, {
        resetKey: `${upid}:${stacked ? 's' : 'l'}`,
        series: stripSeries,
        latestSec: Number.isFinite(maxPointT) ? maxPointT : latestSec,
        windowSec: 60,
        lagSec: 2.0,
        isLive: isPlaying,
        stacked,
        height: 280,
        formatY: (v: number) => formatBytesIec(v * 1024),
        showLegend: true,
      });
    };
    const lmks = data.lmkEvents.filter((e) => e.pid === proc.pid);
    const canProfile = proc.debuggable || data.isUserDebug;
    const bb = (counter: string, label: string, desc: string) =>
      m(Billboard, {
        ...billboardBytes(latest(data, upid, counter)),
        label,
        desc,
      });

    return [
      m(BillboardStrip, [
        bb('mem.rss', 'RSS', 'Resident set size'),
        bb('mem.rss.anon', 'Anon', 'Anonymous resident memory'),
        bb('mem.rss.file', 'File', 'File-backed resident memory'),
        bb('mem.rss.shmem', 'Shmem', 'Shared memory'),
        bb('mem.swap', 'Swap', 'Swapped-out memory'),
      ]),
      m(
        Panel,
        m(Panel.Header, {
          title: 'Capture',
          subtitle:
            'Record a native heap profile (heapprofd) with periodic Java ' +
            'heap dumps. Stopping opens the result in Memory Explorer.',
          controls: m(Button, {
            label: 'Record heap profile',
            icon: canProfile ? 'fiber_manual_record' : 'warning',
            variant: ButtonVariant.Filled,
            intent: Intent.Primary,
            tooltip: canProfile
              ? undefined
              : 'Process is not marked debuggable. Profiling may fail if ' +
                'the device is not a userdebug or eng build.',
            onclick: () =>
              session
                .startProfile(proc.pid, proc.processName)
                .then(() => m.redraw()),
          }),
        }),
      ),
      m(
        '.pf-memscope-panel-row',
        m(
          Panel,
          m(Panel.Header, {
            title: 'Memory breakdown',
            help:
              'RSS Anon + Swap, file-backed and DMA-BUF memory, stacked. ' +
              'Source: linux.process_stats polling /proc/<pid>/status.',
          }),
          m(
            Panel.Body,
            chart(buildProcessMemoryBreakdown(data, proc.pid, data.ts0), true),
          ),
        ),
        m(
          Panel,
          m(Panel.Header, {title: 'Memory counters'}),
          m(Panel.Body, chart(buildCounterChart(data, upid), false)),
        ),
      ),
      m(
        Panel,
        m(Panel.Header, {title: `LMK kills (${lmks.length})`}),
        m(
          Panel.Body,
          lmks.length === 0
            ? m('.pf-memscope-placeholder', 'No LMK kills for this process.')
            : lmks.map((e) =>
                m(
                  'div',
                  `${((e.ts - data.ts0) / 1e9).toFixed(1)}s: killed ` +
                    `(oom_score_adj ${e.oomScoreAdj})`,
                ),
              ),
        ),
      ),
      renderSystemSummary(data, true, SYSTEM_CHART_HEIGHT, !session.isPaused),
    ];
  }
}
