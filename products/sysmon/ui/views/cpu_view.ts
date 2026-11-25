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
import type {ColumnSchema} from '../../../../ui/src/components/widgets/datagrid/datagrid_schema';
import {Button, ButtonVariant} from '../../../../ui/src/widgets/button';
import {Intent} from '../../../../ui/src/widgets/common';
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
import {proportionBar} from '../../../common/ui/kit/charts';
import {fmtCount, fmtMs, fmtPct} from '../../../common/ui/kit/format';
import {colorChip} from '../../../common/ui/kit/sparkline';
import {
  chartCard,
  cpuBreakdownChart,
  cpuCoreHeatmapChart,
  cpuCoresBarChart,
  cpuFreqChart,
  cpuPressureChart,
} from '../charts';
import {
  loadCpuBundle,
  loadWaitExplainer,
  type CpuBundle,
  type WaitExplainerRow,
  type WindowArgs,
} from '../queries';
import {sysmonState, type CapturedSnapshot} from '../state';
import {
  codeCell,
  countCell,
  freqKhzCell,
  msCell,
  pctCell,
  processLinkCell,
  simpleGrid,
} from '../tables';

const WAIT_COLORS: Record<string, string> = {
  running: '#16a34a',
  runnable: '#dc2626',
  io_wait: '#0284c7',
  uninterruptible: '#d97706',
};

const THREAD_LEGEND = [
  {label: 'Running', color: '#16a34a'},
  {label: 'Runnable', color: '#dc2626'},
  {label: 'Disk I/O Wait', color: '#0284c7'},
  {label: 'Uninterruptible D', color: '#d97706'},
];

interface CpuClusterSummary {
  readonly name: string;
  readonly cpus: number[];
  readonly avgBusyPct: number;
  readonly avgUserPct: number;
  readonly avgSysPct: number;
  readonly avgFreqKhz: number | null;
  readonly maxFreqKhz: number | null;
  readonly color: string;
}

function buildCoreClusters(
  cores: ReadonlyArray<CpuBundle['cores'][number]>,
): CpuClusterSummary[] {
  if (cores.length === 0) return [];
  const hasFreq = cores.some((c) => c.maxFreqKhz !== null && c.maxFreqKhz > 0);
  const groups: {name: string; list: CpuBundle['cores'][number][]}[] = [];
  if (hasFreq) {
    const byCap = new Map<number, CpuBundle['cores'][number][]>();
    for (const c of cores) {
      // Round maxFreq to nearest 50 MHz bucket so identical cores group cleanly.
      const cap =
        c.maxFreqKhz !== null ? Math.round(c.maxFreqKhz / 50_000) * 50_000 : 0;
      let arr = byCap.get(cap);
      if (!arr) {
        arr = [];
        byCap.set(cap, arr);
      }
      arr.push(c);
    }
    const tiers = [...byCap.entries()].sort((a, b) => a[0] - b[0]);
    const tierNames =
      tiers.length === 1
        ? ['All cores']
        : tiers.length === 2
          ? ['Little cores', 'Big cores']
          : tiers.length === 3
            ? ['Little cores', 'Mid cores', 'Big cores']
            : tiers.map((_, i) => `Cluster ${i + 1}`);
    tiers.forEach(([, list], i) =>
      groups.push({name: tierNames[i] ?? `Cluster ${i + 1}`, list}),
    );
  } else if (cores.length === 8) {
    groups.push(
      {name: 'Little cores', list: cores.filter((c) => c.cpu < 4)},
      {name: 'Mid cores', list: cores.filter((c) => c.cpu >= 4 && c.cpu < 7)},
      {name: 'Big core', list: cores.filter((c) => c.cpu === 7)},
    );
  } else {
    groups.push({name: 'All cores', list: [...cores]});
  }

  const tierColors = ['#0891b2', '#2563eb', '#7c3aed', '#ea580c'];

  return groups
    .filter((g) => g.list.length > 0)
    .map(({name, list}, idx) => {
      const avg = (fn: (c: CpuBundle['cores'][number]) => number | null) =>
        list.reduce((a, c) => a + (fn(c) ?? 0), 0) / Math.max(1, list.length);
      const freqVals = list
        .map((c) => c.avgFreqKhz)
        .filter((v): v is number => v !== null && v > 0);
      const maxVals = list
        .map((c) => c.maxFreqKhz)
        .filter((v): v is number => v !== null && v > 0);
      return {
        name,
        cpus: list.map((c) => c.cpu).sort((a, b) => a - b),
        avgBusyPct: avg((c) => c.busyPct),
        avgUserPct: avg((c) => c.userPct),
        avgSysPct: avg((c) => c.sysPct),
        avgFreqKhz:
          freqVals.length > 0
            ? freqVals.reduce((a, b) => a + b, 0) / freqVals.length
            : null,
        maxFreqKhz: maxVals.length > 0 ? Math.max(...maxVals) : null,
        color: tierColors[idx % tierColors.length]!,
      };
    });
}

function fmtGhz(khz: number | null): string {
  if (khz === null || khz <= 0) return '—';
  return `${(khz / 1_000_000).toFixed(2)} GHz`;
}

// Lazily loads wait clusters when a wait category row is expanded inline in CPU tab.
class InlineWaitDetail
  implements
    m.ClassComponent<{
      readonly snap: CapturedSnapshot;
      readonly category: 'runnable' | 'io_wait' | 'uninterruptible';
      readonly win?: WindowArgs;
    }>
{
  private readonly memo = new AsyncMemo<WaitExplainerRow[]>();

  onremove() {
    this.memo.dispose();
  }

  view({
    attrs: {snap, category, win},
  }: m.CVnode<{
    readonly snap: CapturedSnapshot;
    readonly category: 'runnable' | 'io_wait' | 'uninterruptible';
    readonly win?: WindowArgs;
  }>) {
    const rows = this.memo.use({
      key: `${snap.id}:${category}:${win?.startTs?.toString() ?? 'all'}:${win?.endTs?.toString() ?? 'all'}`,
      compute: () => loadWaitExplainer(snap.engine, category, win, 10),
    }).data;

    if (!rows) {
      return m('.pf-sysmon-wait', {style: {padding: '8px 0'}}, [
        m(Spinner, {easing: true}),
        m('span', 'Loading blocked kernel functions and wakers…'),
      ]);
    }
    if (rows.length === 0) {
      return m('.pf-sysmon-muted', 'No wait clusters recorded for this window.');
    }

    // Aggregate top blocked kernel functions, top wakers & top stalled processes inline.
    const byFn = new Map<string, number>();
    const byWaker = new Map<string, number>();
    const byProc = new Map<string, number>();
    const isUnknown = (s: string | null | undefined) =>
      !s ||
      s === '—' ||
      s === 'unknown' ||
      s.startsWith('(unsymbolized');
    for (const r of rows) {
      byProc.set(r.processName, (byProc.get(r.processName) ?? 0) + r.totalMs);
      if (category !== 'runnable' && !isUnknown(r.blockedFunction)) {
        byFn.set(r.blockedFunction, (byFn.get(r.blockedFunction) ?? 0) + r.totalMs);
      }
      if (!isUnknown(r.topWaker)) {
        byWaker.set(r.topWaker, (byWaker.get(r.topWaker) ?? 0) + r.totalMs);
      }
    }
    const topFn = [...byFn.entries()].sort((a, b) => b[1] - a[1])[0];
    const topWaker = [...byWaker.entries()].sort((a, b) => b[1] - a[1])[0];
    const topProc = [...byProc.entries()].sort((a, b) => b[1] - a[1])[0];
    const longest = Math.max(...rows.map((r) => r.maxMs));

    return [
      m('.pf-prod-exp__parts', [
        detailPartCard({
          color: WAIT_COLORS[category] ?? '#2563eb',
          label: 'Longest single wait',
          value: fmtMs(longest),
          sub: `${rows.length} clusters`,
          detail: `Top stalled thread: ${rows[0]!.processName}/${rows[0]!.threadName}`,
        }),
        topProc &&
          detailPartCard({
            color: '#2563eb',
            label: 'Most affected process',
            value: fmtMs(topProc[1]),
            detail: m('code', topProc[0]),
          }),
        topFn &&
          detailPartCard({
            color: '#7c3aed',
            label: 'Top blocked function',
            value: fmtMs(topFn[1]),
            detail: m('code', topFn[0]),
          }),
        topWaker &&
          detailPartCard({
            color: '#0891b2',
            label: 'Top waker',
            value: fmtMs(topWaker[1]),
            detail: m('code', topWaker[0]),
          }),
      ]),
      simpleGrid(
        {
          process: {
            title: 'Stalled Process',
            columnType: 'text',
            cellRenderer: processLinkCell((name) =>
              sysmonState.openSnapSubtab(snap, 'process', name),
            ),
          },
          thread: {title: 'Thread', columnType: 'text', cellRenderer: codeCell},
          blocked_fn: {title: 'Blocked Fn', columnType: 'text', cellRenderer: codeCell},
          waker: {title: 'Waker', columnType: 'text', cellRenderer: codeCell},
          total_ms: {title: 'Total', columnType: 'quantitative', cellRenderer: msCell},
          max_ms: {title: 'Max', columnType: 'quantitative', cellRenderer: msCell},
        },
        rows.slice(0, 6).map((r) => ({
          process: r.processName,
          thread: r.threadName,
          blocked_fn: r.blockedFunction,
          waker: r.topWaker,
          total_ms: r.totalMs,
          max_ms: r.maxMs,
        })),
      ),
      m('.pf-prod-exp__actions', [
        m(Button, {
          label: 'Open full Wait Explainer tab',
          rightIcon: 'arrow_forward',
          variant: ButtonVariant.Outlined,
          compact: true,
          onclick: () => sysmonState.openSnapSubtab(snap, 'wait', category),
        }),
      ]),
    ];
  }
}

export function SnapshotCpuView(): m.Component<{
  readonly snap: CapturedSnapshot;
}> {
  const memo = new AsyncMemo<CpuBundle>();
  // Track explicit user toggles so smart defaults hold until clicked.
  const panelToggles = new Map<string, boolean>();
  const expandedWaits = new Set<string>();
  const expandedThreads = new Set<string>();
  let showCoreGrid = false;
  let showThreadGrid = false;

  const isOpen = (id: string, defaultOpen: boolean) =>
    panelToggles.get(id) ?? defaultOpen;
  const togglePanel = (id: string, defaultOpen: boolean) => {
    panelToggles.set(id, !isOpen(id, defaultOpen));
  };

  return {
    view({attrs: {snap}}) {
      const win = sysmonState.getWindowArgs(snap);
      const d = memo.use({
        key: {
          id: snap.id,
          s: win?.startTs?.toString() ?? 'all',
          e: win?.endTs?.toString() ?? 'all',
        },
        compute: () => loadCpuBundle(snap.engine, win, 25),
      }).data;

      if (!d) {
        return m('.pf-sysmon-wait', [
          m(Spinner, {easing: true}),
          m('span', 'Loading CPU & scheduling view…'),
        ]);
      }

      const avgBusy =
        d.cores.length > 0
          ? d.cores.reduce((a, c) => a + (c.busyPct ?? 0), 0) / d.cores.length
          : null;
      const busiestCore = [...d.cores].sort(
        (a, b) => (b.busyPct ?? 0) - (a.busyPct ?? 0),
      )[0];
      const runnable = d.waits.find((w) => w.category === 'runnable');
      const ioWait = d.waits.find((w) => w.category === 'io_wait');
      const dOther = d.waits.find((w) => w.category === 'uninterruptible');
      const nonRunWaits = d.waits.filter((w) => w.category !== 'running');
      const totalWaitMs = nonRunWaits.reduce((a, w) => a + w.totalMs, 0);
      const maxWaitMs = Math.max(1, ...nonRunWaits.map((w) => w.totalMs));
      const clusters = buildCoreClusters(d.cores);

      const coreSchema: ColumnSchema = {
        cpu: {title: 'CPU', columnType: 'text'},
        busy_pct: {title: 'Busy %', columnType: 'quantitative', cellRenderer: pctCell},
        user_pct: {title: 'User %', columnType: 'quantitative', cellRenderer: pctCell},
        sys_pct: {title: 'Sys %', columnType: 'quantitative', cellRenderer: pctCell},
        iowait_pct: {title: 'IO Wait %', columnType: 'quantitative', cellRenderer: pctCell},
        avg_freq_khz: {title: 'Avg Freq', columnType: 'quantitative', cellRenderer: freqKhzCell},
        max_freq_khz: {title: 'Max Freq', columnType: 'quantitative', cellRenderer: freqKhzCell},
      };

      const threadSchema: ColumnSchema = {
        process: {
          title: 'Process',
          columnType: 'text',
          cellRenderer: processLinkCell((name) =>
            sysmonState.openSnapSubtab(snap, 'process', name),
          ),
        },
        thread: {title: 'Thread', columnType: 'text'},
        running_ms: {title: 'Running', columnType: 'quantitative', cellRenderer: msCell},
        runnable_ms: {title: 'Runnable', columnType: 'quantitative', cellRenderer: msCell},
        io_wait_ms: {title: 'I/O Wait', columnType: 'quantitative', cellRenderer: msCell},
        d_other_ms: {title: 'Unint. D', columnType: 'quantitative', cellRenderer: msCell},
      };

      const maxThreadMs = Math.max(
        1,
        ...d.topThreads.slice(0, 15).map((t) => t.totalActiveMs),
      );
      const peakClusterFreq = clusters.reduce(
        (m, c) => Math.max(m, c.maxFreqKhz ?? 0),
        0,
      );
      const clusterSummaryText = clusters
        .map((c) => {
          const range =
            c.cpus.length === 1
              ? `CPU ${c.cpus[0]}`
              : `CPU ${c.cpus[0]}–${c.cpus[c.cpus.length - 1]}`;
          const freqStr =
            c.avgFreqKhz !== null ? `, avg ${fmtGhz(c.avgFreqKhz)}` : '';
          return `${c.name} (${range}): ${fmtPct(c.avgBusyPct)} busy${freqStr}`;
        })
        .join(' · ');

      return m('.pf-prod-landing', [
        // Executive summary card
        questionBlock(
          {
            question: 'CPU & scheduling diagnosis',
            answer: [
              `Average utilization was ${fmtPct(avgBusy)} across ${d.cores.length} cores` +
                (busiestCore
                  ? ` (busiest: CPU ${busiestCore.cpu} at ${fmtPct(busiestCore.busyPct)}).`
                  : '.'),
              runnable && runnable.sharePct >= 10
                ? `Runnable contention accounted for ${fmtPct(runnable.sharePct)} of active thread time (${fmtMs(runnable.totalMs)}).`
                : undefined,
              ioWait && ioWait.sharePct >= 8
                ? `Disk / I/O Wait stalled threads for ${fmtMs(ioWait.totalMs)} (${fmtPct(ioWait.sharePct)}).`
                : undefined,
            ],
            answerIntent:
              (runnable?.sharePct ?? 0) >= 25 || (avgBusy ?? 0) >= 75
                ? Intent.Danger
                : (runnable?.sharePct ?? 0) >= 12 || (avgBusy ?? 0) >= 45
                  ? Intent.Warning
                  : Intent.Primary,
          },
          [
            m('.pf-prod-stats', [
              stat(
                'Avg CPU busy',
                fmtPct(avgBusy),
                busiestCore ? `Peak core: CPU ${busiestCore.cpu} (${fmtPct(busiestCore.busyPct)})` : `${d.cores.length} cores`,
                (avgBusy ?? 0) >= 75 ? 'bad' : (avgBusy ?? 0) >= 45 ? 'warn' : undefined,
              ),
              stat(
                'Runnable wait',
                runnable ? fmtMs(runnable.totalMs) : '0 ms',
                runnable ? `${fmtPct(runnable.sharePct)} of active thread time` : 'No runnable contention',
                (runnable?.sharePct ?? 0) >= 20 ? 'bad' : (runnable?.sharePct ?? 0) >= 10 ? 'warn' : undefined,
              ),
              stat(
                'Disk I/O wait',
                ioWait ? fmtMs(ioWait.totalMs) : '0 ms',
                ioWait ? `${fmtPct(ioWait.sharePct)} · ${fmtCount(ioWait.eventCount)} waits` : 'Quiet',
                (ioWait?.totalMs ?? 0) >= 2000 ? 'warn' : undefined,
              ),
              stat(
                'Uninterruptible D',
                dOther ? fmtMs(dOther.totalMs) : '0 ms',
                dOther ? `${fmtCount(dOther.eventCount)} lock/reclaim waits` : 'Quiet',
              ),
            ]),
            d.waits.length > 0 &&
              proportionBar(
                d.waits.map((w) => ({
                  label: w.label,
                  value: w.totalMs,
                  display: `${fmtMs(w.totalMs)} · ${fmtPct(w.sharePct)}`,
                  color: WAIT_COLORS[w.category] ?? '#64748b',
                })),
              ),
          ],
        ),

        // Panel 1: Thread Wait & Contention Forensics (expandable wait rows!)
        nonRunWaits.length > 0 &&
          collapsiblePanel(
            {
              title: 'Wait & Contention Forensics',
              icon: 'hourglass_top',
              color: '#d97706',
              metric: fmtMs(totalWaitMs),
              metricSub: `${nonRunWaits.length} wait categories`,
              badge:
                (runnable?.sharePct ?? 0) >= 15
                  ? {label: 'Contended', tone: 'warn'}
                  : {label: 'Inspectable', tone: 'muted'},
              summary:
                'Click any wait category below to expand stalled threads, top blocked kernel functions, and wakers right inline.',
              open: isOpen('waits', true),
              onToggle: () => togglePanel('waits', true),
            },
            () =>
              m(
                '.pf-prod-exp',
                nonRunWaits.map((w) => {
                  const cat = w.category as
                    | 'runnable'
                    | 'io_wait'
                    | 'uninterruptible';
                  const open = expandedWaits.has(cat);
                  return expandableRow(
                    {
                      key: `wait-${cat}`,
                      open,
                      onToggle: () => {
                        if (open) expandedWaits.delete(cat);
                        else expandedWaits.add(cat);
                      },
                      title: m('strong', w.label),
                      subtitle: `${fmtCount(w.eventCount)} waits · click to inspect blocked functions & wakers`,
                      primaryStat: fmtMs(w.totalMs),
                      secondaryStat: fmtPct(w.sharePct),
                      segments: [
                        {
                          label: w.label,
                          value: w.totalMs,
                          display: fmtMs(w.totalMs),
                          color: WAIT_COLORS[cat] ?? '#64748b',
                        },
                      ],
                      maxTotal: maxWaitMs,
                      accentColor: WAIT_COLORS[cat],
                    },
                    () => m(InlineWaitDetail, {snap, category: cat, win}),
                  );
                }),
              ),
          ),

        // Panel 2: Core Utilization & Core×Time Heatmap
        (d.series.length > 1 || d.coreSeries.length > 0) &&
          collapsiblePanel(
            {
              title: 'Core Utilization & Timeline',
              icon: 'speed',
              color: '#2563eb',
              metric: fmtPct(avgBusy),
              metricSub: `${d.cores.length} cores`,
              summary:
                'System CPU breakdown over time paired with the per-core Core × Time saturation heatmap.',
              open: isOpen('capacity', true),
              onToggle: () => togglePanel('capacity', true),
            },
            () => [
              d.series.length > 1 &&
                m('.pf-sysmon-chart-row', [
                  chartCard(
                    'CPU time breakdown (stacked)',
                    cpuBreakdownChart(d.series, 200),
                  ),
                  chartCard(
                    'Utilization vs CPU PSI',
                    cpuPressureChart(d.series, 200),
                  ),
                ]),
              d.coreSeries.length > 0 &&
                chartCard(
                  'Per-core utilization over time (Core × Time heatmap)',
                  cpuCoreHeatmapChart(d.coreSeries),
                ),
            ],
          ),

        // Panel 3: CPU Clusters, Frequency Scaling & Per-Core Table (collapsed by default!)
        d.cores.length > 0 &&
          collapsiblePanel(
            {
              title: 'CPU Clusters, Frequency Scaling & Per-Core Stats',
              icon: 'memory',
              color: '#7c3aed',
              metric: `${clusters.length} ${clusters.length === 1 ? 'cluster' : 'clusters'}`,
              metricSub: peakClusterFreq > 0 ? `peak ${fmtGhz(peakClusterFreq)}` : undefined,
              summary: clusterSummaryText,
              quiet: true,
              open: isOpen('clusters', false),
              onToggle: () => togglePanel('clusters', false),
            },
            () => [
              clusters.length > 0 &&
                m(
                  '.pf-prod-exp__parts',
                  clusters.map((c) =>
                    detailPartCard({
                      color: c.color,
                      label: `${c.name} (CPU ${c.cpus.join(',')})`,
                      value: fmtPct(c.avgBusyPct),
                      sub:
                        c.maxFreqKhz !== null
                          ? `max ${fmtGhz(c.maxFreqKhz)}`
                          : `${c.cpus.length} ${c.cpus.length === 1 ? 'core' : 'cores'}`,
                      detail:
                        c.avgFreqKhz !== null
                          ? `Avg freq ${fmtGhz(c.avgFreqKhz)} · User ${fmtPct(c.avgUserPct)} · Sys ${fmtPct(c.avgSysPct)}`
                          : `User ${fmtPct(c.avgUserPct)} · Kernel ${fmtPct(c.avgSysPct)}`,
                    }),
                  ),
                ),
              m('.pf-sysmon-chart-row', [
                d.freqSeries.length >= 2 &&
                  chartCard(
                    'CPU cluster frequency over time',
                    cpuFreqChart(d.freqSeries, 195),
                  ),
                chartCard(
                  'Per-core average breakdown',
                  cpuCoresBarChart(d.cores, 195),
                ),
              ]),
              disclosureMore(
                {
                  open: showCoreGrid,
                  label: `Show per-core table (${d.cores.length} CPUs)`,
                  openLabel: 'Hide per-core table',
                  onToggle: () => {
                    showCoreGrid = !showCoreGrid;
                  },
                },
                () =>
                  simpleGrid(
                    coreSchema,
                    d.cores.map((c) => ({
                      cpu: `CPU ${c.cpu}`,
                      busy_pct: c.busyPct,
                      user_pct: c.userPct,
                      sys_pct: c.sysPct,
                      iowait_pct: c.iowaitPct,
                      avg_freq_khz: c.avgFreqKhz,
                      max_freq_khz: c.maxFreqKhz,
                    })),
                  ),
              ),
            ],
          ),

        // Panel 4: Contended & Busiest Threads (expandable ribbon rows)
        d.topThreads.length > 0 &&
          collapsiblePanel(
            {
              title: 'Contended & Busiest Threads',
              icon: 'view_timeline',
              color: '#0891b2',
              metric: `${d.topThreads.length} threads`,
              metricSub: d.topThreads[0]
                ? `top: ${d.topThreads[0].processName}/${d.topThreads[0].threadName}`
                : undefined,
              summary:
                'Threads ranked by total active + wait time. Expand any thread to inspect exact state split and CPU delay ratio.',
              open: isOpen('threads', true),
              onToggle: () => togglePanel('threads', true),
            },
            () => [
              ribbonLegend(
                THREAD_LEGEND,
                'Shared time scale · click any thread to inspect state breakdown',
              ),
              m(
                '.pf-prod-exp',
                d.topThreads.slice(0, 15).map((t) => {
                  const key = `thr-${t.tid}-${t.threadName}`;
                  const open = expandedThreads.has(key);
                  const delayRatio =
                    t.runningMs >= 0.5 ? t.runnableMs / t.runningMs : 0;
                  return expandableRow(
                    {
                      key,
                      open,
                      onToggle: () => {
                        if (open) expandedThreads.delete(key);
                        else expandedThreads.add(key);
                      },
                      title: m('code', t.threadName),
                      subtitle: `${t.processName} · TID ${t.tid}`,
                      primaryStat: fmtMs(t.totalActiveMs),
                      secondaryStat: `${fmtMs(t.runningMs)} run`,
                      chips: [
                        delayRatio >= 0.5 &&
                          colorChip(
                            `${delayRatio.toFixed(1)}× delay`,
                            delayRatio >= 1.0 ? '#dc2626' : '#d97706',
                          ),
                      ],
                      segments: [
                        {label: 'Running', value: t.runningMs, display: fmtMs(t.runningMs), color: '#16a34a'},
                        {label: 'Runnable', value: t.runnableMs, display: fmtMs(t.runnableMs), color: '#dc2626'},
                        {label: 'I/O Wait', value: t.ioWaitMs, display: fmtMs(t.ioWaitMs), color: '#0284c7'},
                        {label: 'D-other', value: t.dOtherMs, display: fmtMs(t.dOtherMs), color: '#d97706'},
                      ],
                      maxTotal: maxThreadMs,
                      accentColor: delayRatio >= 1.0 ? '#dc2626' : '#2563eb',
                    },
                    () => [
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
                          sub: t.runningMs >= 0.5 ? `${delayRatio.toFixed(2)}× delay` : undefined,
                          detail:
                            delayRatio >= 1.0
                              ? 'Spent longer waiting in runqueue than executing on CPU.'
                              : undefined,
                        }),
                        detailPartCard({
                          color: '#0284c7',
                          label: 'Disk I/O Wait',
                          value: fmtMs(t.ioWaitMs),
                        }),
                        detailPartCard({
                          color: '#d97706',
                          label: 'Uninterruptible D',
                          value: fmtMs(t.dOtherMs),
                        }),
                      ]),
                      m('.pf-prod-exp__actions', [
                        m(Button, {
                          label: `Open ${t.processName} tab`,
                          rightIcon: 'arrow_forward',
                          variant: ButtonVariant.Outlined,
                          compact: true,
                          onclick: () =>
                            sysmonState.openSnapSubtab(snap, 'process', t.processName),
                        }),
                      ]),
                    ],
                  );
                }),
              ),
              disclosureMore(
                {
                  open: showThreadGrid,
                  label: `Show full thread table (${d.topThreads.length} threads)`,
                  openLabel: 'Hide full thread table',
                  onToggle: () => {
                    showThreadGrid = !showThreadGrid;
                  },
                },
                () =>
                  simpleGrid(
                    threadSchema,
                    d.topThreads.map((t) => ({
                      process: t.processName,
                      thread: `${t.threadName} (${t.tid})`,
                      running_ms: t.runningMs,
                      runnable_ms: t.runnableMs,
                      io_wait_ms: t.ioWaitMs,
                      d_other_ms: t.dOtherMs,
                    })),
                  ),
              ),
            ],
          ),

        limitCallout({
          badge: 'Latency',
          title: 'Need operation-level critical paths or callstacks?',
          body: 'Sysmon covers whole-device CPU contention and thread waits. Use lat for operation-level critical paths and function attribution.',
        }),
      ]);
    },
    onremove() {
      memo.dispose();
    },
  };
}

// Wait detail view opened from CPU wait cards.
export function SnapshotWaitExplainerView(): m.Component<{
  readonly snap: CapturedSnapshot;
  readonly category: 'runnable' | 'io_wait' | 'uninterruptible';
}> {
  const memo = new AsyncMemo<WaitExplainerRow[]>();

  return {
    view({attrs: {snap, category}}) {
      const win = sysmonState.getWindowArgs(snap);
      const rows = memo.use({
        key: {
          id: snap.id,
          category,
          s: win?.startTs?.toString() ?? 'all',
          e: win?.endTs?.toString() ?? 'all',
        },
        compute: () => loadWaitExplainer(snap.engine, category, win, 30),
      }).data;

      if (!rows) {
        return m('.pf-sysmon-wait', [
          m(Spinner, {easing: true}),
          m('span', 'Loading wait breakdown…'),
        ]);
      }

      const titleMap: Record<string, string> = {
        runnable: 'Runnable (CPU contention)',
        io_wait: 'Disk / I/O Wait (D-state)',
        uninterruptible: 'Uninterruptible Sleep (locks / reclaim)',
      };
      const top = rows[0];
      const totalMs = rows.reduce((a, r) => a + r.totalMs, 0);

      const schema: ColumnSchema = {
        process: {
          title: 'Process',
          columnType: 'text',
          cellRenderer: processLinkCell((name) =>
            sysmonState.openSnapSubtab(snap, 'process', name),
          ),
        },
        thread: {title: 'Thread', columnType: 'text'},
        blocked_fn: {
          title: 'Blocked Function',
          columnType: 'text',
          cellRenderer: codeCell,
        },
        waker: {title: 'Top Waker', columnType: 'text', cellRenderer: codeCell},
        wait_count: {title: 'Waits', columnType: 'quantitative', cellRenderer: countCell},
        total_ms: {title: 'Total', columnType: 'quantitative', cellRenderer: msCell},
        max_ms: {title: 'Max', columnType: 'quantitative', cellRenderer: msCell},
      };

      return m('.pf-prod-landing', [
        questionBlock(
          {
            question: `Why were threads in ${titleMap[category] ?? category}?`,
            answer: top
              ? `Top wait cluster: \`${top.processName}/${top.threadName}\` spent ${fmtMs(top.totalMs)} across ${fmtCount(top.waitCount)} waits (max ${fmtMs(top.maxMs)}), blocked in \`${top.blockedFunction}\`, woken by \`${top.topWaker}\`.`
              : 'No waits in this category for the selected window.',
          },
          [
            m('.pf-prod-stats', [
              stat('Top-30 wait total', fmtMs(totalMs)),
              stat('Clusters', String(rows.length)),
              stat(
                'Longest single wait',
                rows.length > 0
                  ? fmtMs(Math.max(...rows.map((r) => r.maxMs)))
                  : '—',
              ),
            ]),
            rows.length > 0 &&
              simpleGrid(
                schema,
                rows.map((r) => ({
                  process: r.processName,
                  thread: r.threadName,
                  blocked_fn: r.blockedFunction,
                  waker: r.topWaker,
                  wait_count: r.waitCount,
                  total_ms: r.totalMs,
                  max_ms: r.maxMs,
                })),
              ),
          ],
        ),
      ]);
    },
    onremove() {
      memo.dispose();
    },
  };
}
