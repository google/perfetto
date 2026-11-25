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
import {AsyncMemo} from '../../../../ui/src/base/async_memo';
import type {Engine} from '../../../../ui/src/trace_processor/engine';
import {Grid, GridCell, type GridColumn} from '../../../../ui/src/widgets/grid';
import {Spinner} from '../../../../ui/src/widgets/spinner';
import {
  collapsiblePanel,
  detailPartCard,
  disclosureMore,
  expandableRow,
  limitCallout,
  questionBlock,
  ribbonLegend,
  stat,
} from '../../../common/ui/kit/blocks';
import {
  proportionBar,
  type LiveStripContext,
} from '../../../common/ui/kit/charts';
import {
  fmtBytes,
  fmtDeltaBytes,
  fmtMs,
  fmtPct,
  fmtPowerMw,
  fmtTempC,
} from '../../../common/ui/kit/format';
import {
  colorChip,
  formatAgeShort,
  oomBucketFor,
  scorecardTile,
} from '../../../common/ui/kit/sparkline';
import {
  chartCard,
  processCpuChart,
  processMemoryChart,
  runnableHistChart,
} from '../charts';
import {
  loadProcessDetailBundle,
  loadTopProcesses,
  type AppEventRow,
  type ProcessDetailBundle,
  type PulseRow,
  type TopProcessRow,
  type WindowArgs,
} from '../queries';
import {sysmonState, type CapturedSnapshot} from '../state';
import {renderEventFeed} from './apps_view';

const THREAD_LEGEND = [
  {label: 'Running', color: '#16a34a'},
  {label: 'Runnable', color: '#dc2626'},
  {label: 'Disk I/O Wait', color: '#0284c7'},
  {label: 'Uninterruptible D', color: '#d97706'},
];

interface SnapshotProcessUiState {
  readonly panelToggles: Map<string, boolean>;
  readonly expandedThreads: Set<number>;
  showThreadGrid: boolean;
}

// Shared body for a closable Process tab (used in both Live and Snapshot).
// Snapshot mode uses structured collapsible panels & expandable thread ribbons;
// Live mode stays concise and glanceable.
function renderProcessBody(opts: {
  readonly mode: 'live' | 'snapshot';
  readonly processName: string;
  readonly row?: TopProcessRow;
  readonly detail?: ProcessDetailBundle;
  readonly detailPending: boolean;
  readonly events?: ReadonlyArray<AppEventRow>;
  readonly devicePulse?: PulseRow;
  readonly liveCtx?: LiveStripContext;
  readonly uiState?: SnapshotProcessUiState;
  readonly onOpenCpu?: () => void;
  readonly onOpenMemory?: () => void;
  readonly onTakeSnapshot?: () => void;
  readonly onOpenProcess: (name: string) => void;
}): m.Children {
  const {row, detail, processName, uiState} = opts;
  const bucket = row ? oomBucketFor(row.oomScoreAdj) : undefined;
  const procEvents = (opts.events ?? []).filter(
    (e) => e.processName === processName,
  );

  // Thread state aggregate across this process (Snapshot).
  const threads = detail?.threads ?? [];
  const totRun = threads.reduce((s, t) => s + t.runningMs, 0);
  const totRunnable = threads.reduce((s, t) => s + t.runnableMs, 0);
  const totIo = threads.reduce((s, t) => s + t.ioWaitMs, 0);
  const totDOther = threads.reduce((s, t) => s + t.dOtherMs, 0);
  const totActive = totRun + totRunnable + totIo + totDOther;
  const maxThrMs = Math.max(1, ...threads.slice(0, 12).map((t) => t.totalActiveMs));

  const isOpen = (id: string, def: boolean) =>
    uiState?.panelToggles.get(id) ?? def;
  const togglePanel = (id: string, def: boolean) => {
    uiState?.panelToggles.set(id, !isOpen(id, def));
  };

  const threadCols: GridColumn[] = [
    {key: 'thread', header: m(GridCell, 'Thread'), maxInitialWidthPx: 260},
    {key: 'tid', header: m(GridCell, 'TID')},
    {key: 'bar', header: m(GridCell, 'Active split'), maxInitialWidthPx: 180},
    {key: 'run', header: m(GridCell, 'Running')},
    {key: 'runnable', header: m(GridCell, 'Runnable')},
    {key: 'delay', header: m(GridCell, 'CPU delay')},
    {key: 'io', header: m(GridCell, 'I/O Wait')},
    {key: 'blocked', header: m(GridCell, 'Top blocked fn')},
  ];

  const threadRows = threads.map((t) => {
    const delayColor =
      t.cpuDelayRatio >= 1.0
        ? 'var(--pf-color-danger)'
        : t.cpuDelayRatio >= 0.5
          ? 'var(--pf-color-warning)'
          : 'var(--pf-color-success)';
    const bar = proportionBar([
      {label: 'Running', value: t.runningMs, display: fmtMs(t.runningMs), color: '#16a34a'},
      {label: 'Runnable', value: t.runnableMs, display: fmtMs(t.runnableMs), color: '#dc2626'},
      {label: 'I/O', value: t.ioWaitMs, display: fmtMs(t.ioWaitMs), color: '#0284c7'},
      {label: 'D-other', value: t.dOtherMs, display: fmtMs(t.dOtherMs), color: '#d97706'},
    ]);
    return [
      m(GridCell, m('code', t.threadName)),
      m(GridCell, {align: 'right'}, `${t.tid}`),
      m(GridCell, m('.pf-sysmon-inline-bar', bar)),
      m(GridCell, {align: 'right'}, fmtMs(t.runningMs)),
      m(GridCell, {align: 'right'}, t.runnableMs > 0 ? fmtMs(t.runnableMs) : '—'),
      m(
        GridCell,
        {align: 'right', style: {color: delayColor, fontWeight: '600'}},
        t.runningMs >= 0.5 ? `${t.cpuDelayRatio.toFixed(2)}×` : '—',
      ),
      m(GridCell, {align: 'right'}, t.ioWaitMs > 0 ? fmtMs(t.ioWaitMs) : '—'),
      m(GridCell, t.topBlockedFn ? m('code', t.topBlockedFn) : '—'),
    ];
  });

  const dp = opts.devicePulse;
  const worstDelayThread = [...threads].sort(
    (a, b) => b.cpuDelayRatio - a.cpuDelayRatio,
  )[0];

  return m('.pf-prod-landing', [
    questionBlock(
      {
        question: `Process · ${processName}`,
        answer: row
          ? `PID ${row.pid} · Age ${formatAgeShort(row.ageS)} · CPU ${fmtPct(row.cpuPct)} · Anon + Swap ${row.anonSwapMib.toFixed(1)} MiB (${fmtDeltaBytes(row.memDeltaMib * 1024 * 1024)})${row.dmabufMib > 0.1 ? ` · DMA-BUF ${row.dmabufMib.toFixed(1)} MiB` : ''}.`
          : `No active samples for \`${processName}\` in this window yet.`,
        actions: [
          ...(opts.onTakeSnapshot
            ? [
                {
                  label: 'Take Snapshot',
                  icon: 'photo_camera',
                  onclick: opts.onTakeSnapshot,
                },
              ]
            : []),
          ...(opts.onOpenMemory
            ? [{label: 'Memory tab', onclick: opts.onOpenMemory}]
            : []),
          ...(opts.onOpenCpu
            ? [{label: 'CPU tab', onclick: opts.onOpenCpu}]
            : []),
          {
            label: 'Open in Memscope',
            icon: 'open_in_new',
            onclick: () => void sysmonState.openInMemscope(processName),
          },
        ],
      },
      row &&
        m('.pf-prod-scorecards', [
          scorecardTile({
            label: 'CPU',
            icon: 'speed',
            value: fmtPct(row.cpuPct),
            sub:
              opts.mode === 'snapshot'
                ? `${fmtMs(row.cpuMs)} run · ${fmtMs(row.runnableMs)} runnable`
                : `${fmtMs(row.cpuMs)} in window`,
            sparkline: row.cpuTrend,
            sparkColor: '#2563eb',
            status: row.cpuPct >= 30 ? 'warn' : undefined,
          }),
          scorecardTile({
            label: 'Anon + Swap',
            icon: 'memory',
            value: `${row.anonSwapMib.toFixed(1)} MiB`,
            sub: `Anon ${row.anonMib.toFixed(1)} · Swap ${row.swapMib.toFixed(1)} · Δ ${fmtDeltaBytes(row.memDeltaMib * 1024 * 1024)}`,
            sparkline: row.ramTrend,
            sparkColor: '#7c3aed',
            status: row.memDeltaMib >= 32 ? 'warn' : undefined,
          }),
          scorecardTile({
            label: 'File RSS',
            icon: 'description',
            value: `${row.fileMib.toFixed(1)} MiB`,
            sub: 'File-backed resident pages',
          }),
          scorecardTile({
            label: 'DMA-BUF',
            icon: 'layers',
            value: row.dmabufMib > 0.05 ? `${row.dmabufMib.toFixed(1)} MiB` : '0 MiB',
            sub: 'Shared GPU / camera / TPU buffers',
          }),
          scorecardTile({
            label: 'State / OOM',
            icon: 'apps',
            value: row.isTop
              ? 'Top App'
              : bucket?.short ?? (row.oomScoreAdj !== null ? `${row.oomScoreAdj}` : '—'),
            sub: `oom_score_adj ${row.oomScoreAdj ?? '—'} · Age ${formatAgeShort(row.ageS)}`,
          }),
          opts.mode === 'snapshot'
            ? scorecardTile({
                label: 'Waits',
                icon: 'hourglass_top',
                value: fmtMs(row.runnableMs + row.ioWaitMs),
                sub: `${fmtMs(row.runnableMs)} runnable · ${fmtMs(row.ioWaitMs)} I/O`,
                status: row.runnableMs + row.ioWaitMs >= 250 ? 'warn' : undefined,
              })
            : scorecardTile({
                label: 'Dominant',
                icon: 'insights',
                value: row.dominantResource,
                sub: row.frozen ? 'Frozen by cgroup freezer' : 'Active process',
              }),
        ]),
    ),

    // Snapshot mode: structured collapsible forensic panels + expandable thread ribbons
    opts.mode === 'snapshot' && uiState
      ? [
          totActive > 0 &&
            collapsiblePanel(
              {
                title: 'Thread Execution & Contention Ribbons',
                icon: 'view_timeline',
                color: '#2563eb',
                metric: `${threads.length} threads`,
                metricSub: `${fmtMs(totRun)} run · ${fmtMs(totRunnable)} runnable · ${fmtMs(totIo)} I/O`,
                badge:
                  worstDelayThread && worstDelayThread.cpuDelayRatio >= 1.0
                    ? {label: `Max ${worstDelayThread.cpuDelayRatio.toFixed(1)}× delay`, tone: 'warn'}
                    : undefined,
                summary:
                  'Expand any thread to inspect exact state split, CPU delay ratio (runnable/running), and top blocked kernel function.',
                open: isOpen('threads', true),
                onToggle: () => togglePanel('threads', true),
              },
              () => [
                proportionBar([
                  {label: 'Running', value: totRun, display: fmtMs(totRun), color: '#16a34a'},
                  {label: 'Runnable', value: totRunnable, display: fmtMs(totRunnable), color: '#dc2626'},
                  {label: 'Disk I/O Wait', value: totIo, display: fmtMs(totIo), color: '#0284c7'},
                  {label: 'Uninterruptible', value: totDOther, display: fmtMs(totDOther), color: '#d97706'},
                ]),
                ribbonLegend(
                  THREAD_LEGEND,
                  'Shared thread time scale · click any thread row for details',
                ),
                m(
                  '.pf-prod-exp',
                  threads.slice(0, 12).map((t) => {
                    const open = uiState.expandedThreads.has(t.utid);
                    return expandableRow(
                      {
                        key: `pthr-${t.utid}`,
                        open,
                        onToggle: () => {
                          if (open) uiState.expandedThreads.delete(t.utid);
                          else uiState.expandedThreads.add(t.utid);
                        },
                        title: m('code', t.threadName),
                        subtitle: t.topBlockedFn
                          ? `TID ${t.tid} · blocked: ${t.topBlockedFn}`
                          : `TID ${t.tid}`,
                        primaryStat: fmtMs(t.totalActiveMs),
                        secondaryStat: `${fmtMs(t.runningMs)} run`,
                        chips: [
                          t.runningMs >= 0.5 &&
                            t.cpuDelayRatio >= 0.5 &&
                            colorChip(
                              `${t.cpuDelayRatio.toFixed(1)}× delay`,
                              t.cpuDelayRatio >= 1.0 ? '#dc2626' : '#d97706',
                            ),
                        ],
                        segments: [
                          {label: 'Running', value: t.runningMs, display: fmtMs(t.runningMs), color: '#16a34a'},
                          {label: 'Runnable', value: t.runnableMs, display: fmtMs(t.runnableMs), color: '#dc2626'},
                          {label: 'I/O Wait', value: t.ioWaitMs, display: fmtMs(t.ioWaitMs), color: '#0284c7'},
                          {label: 'D-other', value: t.dOtherMs, display: fmtMs(t.dOtherMs), color: '#d97706'},
                        ],
                        maxTotal: maxThrMs,
                        accentColor: t.cpuDelayRatio >= 1.0 ? '#dc2626' : '#2563eb',
                      },
                      () =>
                        m('.pf-prod-exp__parts', [
                          detailPartCard({
                            color: '#16a34a',
                            label: 'Running',
                            value: fmtMs(t.runningMs),
                            sub: fmtPct((100 * t.runningMs) / Math.max(1, t.totalActiveMs)),
                          }),
                          detailPartCard({
                            color: '#dc2626',
                            label: 'Runnable',
                            value: fmtMs(t.runnableMs),
                            sub:
                              t.runningMs >= 0.5
                                ? `${t.cpuDelayRatio.toFixed(2)}× delay`
                                : undefined,
                          }),
                          detailPartCard({
                            color: '#0284c7',
                            label: 'Disk I/O & D-state',
                            value: fmtMs(t.ioWaitMs + t.dOtherMs),
                            detail: `I/O ${fmtMs(t.ioWaitMs)} · Uninterruptible ${fmtMs(t.dOtherMs)}`,
                          }),
                          t.topBlockedFn &&
                            detailPartCard({
                              color: '#7c3aed',
                              label: 'Top Blocked Function',
                              value: 'Kernel',
                              detail: m('code', t.topBlockedFn),
                            }),
                        ]),
                    );
                  }),
                ),
                disclosureMore(
                  {
                    open: uiState.showThreadGrid,
                    label: `Show full thread table (${threads.length} threads)`,
                    openLabel: 'Hide full thread table',
                    onToggle: () => {
                      uiState.showThreadGrid = !uiState.showThreadGrid;
                    },
                  },
                  () => m(Grid, {columns: threadCols, rowData: threadRows, fillHeight: false}),
                ),
              ],
            ),

          detail &&
            detail.runnableHist.length > 0 &&
            collapsiblePanel(
              {
                title: 'Runnable Wakeup Latency Distribution',
                icon: 'bar_chart',
                color: '#d97706',
                metric: fmtMs(totRunnable),
                metricSub: `${detail.runnableHist.reduce((a, b) => a + b.cnt, 0)} wakeups`,
                summary:
                  'Time spent queued runnable between wakeup and scheduled execution across this process.',
                quiet: totRunnable < 50,
                open: isOpen('hist', totRunnable >= 50),
                onToggle: () => togglePanel('hist', totRunnable >= 50),
              },
              () =>
                chartCard(
                  'Runnable wakeup latency buckets',
                  runnableHistChart(detail.runnableHist, 185),
                ),
            ),

          opts.detailPending && !detail
            ? m('.pf-sysmon-wait', [
                m(Spinner, {easing: true}),
                m('span', `Loading ${processName} timeline…`),
              ])
            : detail &&
              detail.series.length >= 2 &&
              collapsiblePanel(
                {
                  title: 'CPU & Memory Composition Over Time',
                  icon: 'timeline',
                  color: '#7c3aed',
                  metric: row ? `${row.anonSwapMib.toFixed(0)} MiB` : undefined,
                  metricSub: row ? `CPU ${fmtPct(row.cpuPct)}` : undefined,
                  summary:
                    'Per-process CPU share and stacked memory composition (Anon, Swap, File RSS, DMA-BUF, Shmem) across the window.',
                  open: isOpen('timeline', true),
                  onToggle: () => togglePanel('timeline', true),
                },
                () =>
                  m('.pf-sysmon-chart-row', [
                    chartCard('CPU utilization (%)', processCpuChart(detail.series, 205)),
                    chartCard('Memory breakdown', processMemoryChart(detail.series, 205)),
                  ]),
              ),

          procEvents.length > 0 &&
            collapsiblePanel(
              {
                title: 'State Transitions & Lifecycle Events',
                icon: 'notifications',
                color: '#16a34a',
                metric: `${procEvents.length} events`,
                summary: `Recorded OOM state changes and lifecycle transitions for \`${processName}\`.`,
                open: isOpen('events', true),
                onToggle: () => togglePanel('events', true),
              },
              () => renderEventFeed(procEvents, opts.onOpenProcess),
            ),
        ]
      : [
          // Live mode: concise glanceable charts & device context
          opts.detailPending && !detail
            ? m('.pf-sysmon-wait', [
                m(Spinner, {easing: true}),
                m('span', `Loading ${processName} timeline…`),
              ])
            : detail &&
              detail.series.length >= 2 &&
              questionBlock(
                {
                  question: 'CPU & memory over time',
                  answer:
                    'Per-process CPU share and stacked memory composition (Anon, Swap, File RSS, DMA-BUF, Shmem) across the window.',
                },
                m('.pf-sysmon-chart-row', [
                  chartCard(
                    'CPU utilization (%)',
                    processCpuChart(detail.series, 205, opts.liveCtx),
                  ),
                  chartCard(
                    'Memory breakdown',
                    processMemoryChart(detail.series, 205, opts.liveCtx),
                  ),
                ]),
              ),

          procEvents.length > 0 &&
            questionBlock(
              {
                question: 'State transitions & lifecycle events',
                answer: `${procEvents.length} recorded state changes or lifecycle events for \`${processName}\`.`,
              },
              renderEventFeed(procEvents, opts.onOpenProcess),
            ),

          dp &&
            questionBlock(
              {
                question: 'Device context while watching this process',
                answer:
                  'System-wide pressure at the same moment so you can distinguish app-local work from device contention.',
              },
              m('.pf-prod-stats', [
                stat('Device CPU', fmtPct(dp.cpuBusyPct), dp.cpuPsiSomePct !== null ? `PSI ${fmtPct(dp.cpuPsiSomePct)}` : undefined),
                stat(
                  'Mem Available',
                  fmtBytes(dp.memAvailLastBytes),
                  dp.memPsiSomePct !== null ? `Mem PSI ${fmtPct(dp.memPsiSomePct)}` : undefined,
                ),
                stat('Peak Temp', fmtTempC(dp.peakTempC), dp.throttleEvents > 0 ? `${dp.throttleEvents} cooling updates` : 'No throttling'),
                stat('Device Power', fmtPowerMw(dp.avgPowerMw), dp.battPct !== null ? `Battery ${dp.battPct.toFixed(0)}%` : undefined),
              ]),
            ),
        ],

    limitCallout({
      badge: 'Memscope',
      title: `Inspect inside \`${processName}\` with Memscope`,
      body: 'Open Memscope focused on this process to capture Java heap dumps, native heap profiles, and smaps breakdowns.',
      actionLabel: `Open ${processName} in Memscope`,
      onAction: () => void sysmonState.openInMemscope(processName),
    }),
  ]);
}

// Closable live Process tab inside LiveView.
export function LiveProcessTabView(): m.Component<{
  readonly engine?: Engine;
  readonly tickGen: number;
  readonly win?: WindowArgs;
  readonly processName: string;
  readonly row?: TopProcessRow;
  readonly events: ReadonlyArray<AppEventRow>;
  readonly devicePulse?: PulseRow;
  readonly liveCtx?: LiveStripContext;
}> {
  const memo = new AsyncMemo<ProcessDetailBundle>();

  return {
    view({attrs}) {
      const state =
        attrs.engine && attrs.row
          ? memo.use({
              key: `${attrs.tickGen}:${attrs.row.upid}`,
              compute: () =>
                loadProcessDetailBundle(
                  attrs.engine!,
                  attrs.row!.upid,
                  attrs.win,
                  false,
                ),
            })
          : undefined;

      return renderProcessBody({
        mode: 'live',
        processName: attrs.processName,
        row: attrs.row,
        detail: state?.data,
        detailPending: state?.isPending ?? false,
        events: attrs.events,
        devicePulse: attrs.devicePulse,
        liveCtx: attrs.liveCtx,
        onOpenMemory: () => sysmonState.selectLive('memory'),
        onOpenCpu: () => sysmonState.selectLive('cpu'),
        onTakeSnapshot: () => void sysmonState.takeInstantSnapshot(attrs.processName),
        onOpenProcess: (name) => sysmonState.openLiveProcessTab(name),
      });
    },
    onremove() {
      memo.dispose();
    },
  };
}

// Closable Snapshot Process tab inside SnapshotWorkspace.
export function SnapshotProcessView(): m.Component<{
  readonly snap: CapturedSnapshot;
  readonly processName: string;
}> {
  const memo = new AsyncMemo<{
    readonly row?: TopProcessRow;
    readonly detail?: ProcessDetailBundle;
  }>();
  const uiState: SnapshotProcessUiState = {
    panelToggles: new Map<string, boolean>(),
    expandedThreads: new Set<number>(),
    showThreadGrid: false,
  };

  return {
    view({attrs: {snap, processName}}) {
      const win = sysmonState.getWindowArgs(snap);
      const state = memo.use({
        key: `${snap.id}:${processName}:${win?.startTs?.toString() ?? 'all'}:${win?.endTs?.toString() ?? 'all'}`,
        compute: async () => {
          const all = await loadTopProcesses(snap.engine, win, 200);
          const row = all.find((p) => p.processName === processName);
          const detail = row
            ? await loadProcessDetailBundle(snap.engine, row.upid, win, true)
            : undefined;
          return {row, detail};
        },
      });

      if (state.isPending && !state.data) {
        return m('.pf-sysmon-wait', [
          m(Spinner, {easing: true}),
          m('span', `Loading ${processName}…`),
        ]);
      }

      return renderProcessBody({
        mode: 'snapshot',
        processName,
        row: state.data?.row,
        detail: state.data?.detail,
        detailPending: state.isPending,
        devicePulse: snap.pulse,
        uiState,
        onOpenMemory: () => sysmonState.openSnapSubtab(snap, 'memory'),
        onOpenCpu: () => sysmonState.openSnapSubtab(snap, 'cpu'),
        onOpenProcess: (name) => sysmonState.openSnapSubtab(snap, 'process', name),
      });
    },
    onremove() {
      memo.dispose();
    },
  };
}
