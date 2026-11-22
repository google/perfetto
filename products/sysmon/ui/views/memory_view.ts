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
import {
  fmtBytes,
  fmtCount,
  fmtDeltaBytes,
  fmtPct,
} from '../../../common/ui/kit/format';
import {
  colorChip,
  oomBucketByIdx,
  oomBucketFor,
} from '../../../common/ui/kit/sparkline';
import {
  chartCard,
  memoryCompositionChart,
  memoryPagingChart,
  memoryPsiChart,
  oomMemoryStackedChart,
} from '../charts';
import {
  loadAppsBundle,
  loadMemoryBundle,
  type AppsBundle,
  type MemoryBundle,
} from '../queries';
import {sysmonState, type CapturedSnapshot} from '../state';
import {
  codeCell,
  deltaMibCell,
  memscopeActionCell,
  mibCell,
  mibZeroCell,
  processLinkCell,
  simpleGrid,
} from '../tables';

const LEDGER_COLORS: Record<string, string> = {
  'Anon (Processes)': '#7c3aed',
  'File Cache': '#0891b2',
  'Kernel & Slab': '#64748b',
  'DMA-BUF & GPU': '#ec4899',
  ZRAM: '#f59e0b',
  Free: '#16a34a',
  'Other / Reserved': '#94a3b8',
};

const HOLDER_LEGEND = [
  {label: 'Anon', color: '#7c3aed'},
  {label: 'Swap', color: '#f59e0b'},
  {label: 'File RSS', color: '#0891b2'},
  {label: 'DMA-BUF', color: '#ec4899'},
];

interface MemViewData {
  readonly mem: MemoryBundle;
  readonly apps: AppsBundle;
}

export function SnapshotMemoryView(): m.Component<{
  readonly snap: CapturedSnapshot;
}> {
  const memo = new AsyncMemo<MemViewData>();
  const panelToggles = new Map<string, boolean>();
  const expandedHolders = new Set<number>();
  let showFullTable = false;

  const isOpen = (id: string, defaultOpen: boolean) =>
    panelToggles.get(id) ?? defaultOpen;
  const togglePanel = (id: string, defaultOpen: boolean) => {
    panelToggles.set(id, !isOpen(id, defaultOpen));
  };

  return {
    view({attrs: {snap}}) {
      const win = sysmonState.getWindowArgs(snap);
      const data = memo.use({
        key: {
          id: snap.id,
          s: win?.startTs?.toString() ?? 'all',
          e: win?.endTs?.toString() ?? 'all',
        },
        compute: async () => {
          const [mem, apps] = await Promise.all([
            loadMemoryBundle(snap.engine, win, 25),
            loadAppsBundle(snap.engine, win, 10),
          ]);
          return {mem, apps};
        },
      }).data;

      if (!data) {
        return m('.pf-sysmon-wait', [
          m(Spinner, {easing: true}),
          m('span', 'Loading memory view…'),
        ]);
      }

      const d = data.mem;
      const apps = data.apps;
      const r = d.reclaim;
      const topApp = d.topProcs[0];
      const anonRow = d.ledger.find((x) => x.category === 'Anon (Processes)');
      const cacheRow = d.ledger.find((x) => x.category === 'File Cache');
      const dmaRow = d.ledger.find((x) => x.category === 'DMA-BUF & GPU');
      const zramRow = d.ledger.find((x) => x.category === 'ZRAM');
      const freeRow = d.ledger.find((x) => x.category === 'Free');

      const pressureActive =
        (r?.lmkdKills ?? 0) > 0 ||
        (r?.psiMemSomePct ?? 0) >= 1 ||
        (r?.directReclaimEvents ?? 0) > 0 ||
        (r?.pswpinDelta ?? 0) >= 500;

      const lmkdSchema: ColumnSchema = {
        time_s: {
          title: 'Time',
          columnType: 'quantitative',
          cellRenderer: (v: unknown) => ({
            content: typeof v === 'number' ? `${v.toFixed(2)} s` : '—',
            align: 'right' as const,
          }),
        },
        process: {title: 'Killed Process', columnType: 'text', cellRenderer: codeCell},
        pid: {title: 'PID', columnType: 'quantitative'},
        oom_adj: {title: 'OOM adj', columnType: 'quantitative'},
        rss_mib: {title: 'RSS', columnType: 'quantitative', cellRenderer: mibZeroCell},
      };

      const procSchema: ColumnSchema = {
        process: {
          title: 'Process',
          columnType: 'text',
          cellRenderer: processLinkCell((name) =>
            sysmonState.openSnapSubtab(snap, 'process', name),
          ),
        },
        anon_mib: {title: 'Anon', columnType: 'quantitative', cellRenderer: mibZeroCell},
        swap_mib: {title: 'Swap', columnType: 'quantitative', cellRenderer: mibZeroCell},
        file_mib: {title: 'File', columnType: 'quantitative', cellRenderer: mibZeroCell},
        dmabuf_mib: {title: 'DMA-BUF', columnType: 'quantitative', cellRenderer: mibCell},
        delta_mib: {title: 'Δ Anon+Swap', columnType: 'quantitative', cellRenderer: deltaMibCell},
        oom_adj: {title: 'OOM adj', columnType: 'quantitative'},
        memscope: {
          title: '',
          columnType: 'identifier',
          cellRenderer: memscopeActionCell((name) =>
            void sysmonState.openInMemscope(name),
          ),
        },
      };

      const topHolders = d.topProcs.slice(0, 12);
      const maxHolderMib = Math.max(
        1,
        ...topHolders.map((pr) => pr.anonMib + pr.swapMib + pr.fileMib + pr.dmabufMib),
      );
      const oomTotalMib = apps.buckets.reduce((a, b) => a + b.anonSwapMib, 0);
      const topOomBucket = [...apps.buckets].sort(
        (a, b) => b.anonSwapMib - a.anonSwapMib,
      )[0];

      return m('.pf-prod-landing', [
        // Top Verdict & Physical RAM Ledger
        questionBlock(
          {
            question: 'Physical RAM diagnosis',
            answer:
              d.ledger.length > 0
                ? [
                    `Anon working set holds ${fmtBytes(anonRow?.bytes)} (${fmtPct(anonRow?.pctOfTotal)}), File Cache holds ${fmtBytes(cacheRow?.bytes)} (${fmtPct(cacheRow?.pctOfTotal)}), and DMA-BUF & GPU hold ${fmtBytes(dmaRow?.bytes)}.`,
                    zramRow && zramRow.bytes > 0
                      ? `ZRAM compressed swap occupies ${fmtBytes(zramRow.bytes)} (${fmtDeltaBytes(zramRow.deltaBytes)} over window).`
                      : undefined,
                  ]
                : 'This trace does not include /proc/meminfo counters; showing per-process RSS below.',
            answerIntent:
              (r?.lmkdKills ?? 0) > 0
                ? Intent.Danger
                : pressureActive
                  ? Intent.Warning
                  : Intent.Primary,
          },
          [
            d.ledger.length > 0 && [
              m('.pf-prod-stats', [
                stat(
                  'Anon (Processes)',
                  fmtBytes(anonRow?.bytes),
                  `${fmtPct(anonRow?.pctOfTotal)} · ${fmtDeltaBytes(anonRow?.deltaBytes)}`,
                ),
                stat(
                  'File Cache',
                  fmtBytes(cacheRow?.bytes),
                  `${fmtPct(cacheRow?.pctOfTotal)} · ${fmtDeltaBytes(cacheRow?.deltaBytes)}`,
                ),
                stat(
                  'DMA-BUF & ZRAM',
                  fmtBytes((dmaRow?.bytes ?? 0) + (zramRow?.bytes ?? 0)),
                  `DMA ${fmtBytes(dmaRow?.bytes)} · ZRAM ${fmtBytes(zramRow?.bytes)}`,
                ),
                stat(
                  'Free RAM',
                  fmtBytes(freeRow?.bytes),
                  `${r?.lmkdKills ?? 0} LMKD kills · PSI ${fmtPct(r?.psiMemSomePct)}`,
                  (r?.lmkdKills ?? 0) > 0 ? 'bad' : pressureActive ? 'warn' : undefined,
                ),
              ]),
              proportionBar(
                d.ledger.map((row) => ({
                  label: row.category,
                  value: row.bytes,
                  display: `${fmtBytes(row.bytes)} (${fmtDeltaBytes(row.deltaBytes)})`,
                  color: LEDGER_COLORS[row.category] ?? '#64748b',
                })),
              ),
            ],
          ],
        ),

        // Panel 1: Physical RAM Composition Over Time & Ledger Delta Cards
        d.series.length >= 2 &&
          collapsiblePanel(
            {
              title: 'Physical RAM Composition Over Time',
              icon: 'stacked_line_chart',
              color: '#7c3aed',
              metric: fmtBytes(anonRow?.bytes),
              metricSub: anonRow ? `Anon Δ ${fmtDeltaBytes(anonRow.deltaBytes)}` : undefined,
              summary:
                'Stacked physical memory categories across the trace with LMKD kill markers and per-category deltas.',
              open: isOpen('composition', true),
              onToggle: () => togglePanel('composition', true),
            },
            () => [
              memoryCompositionChart(d.series, d.lmkd, 230),
              d.ledger.length > 0 &&
                m(
                  '.pf-prod-exp__parts',
                  d.ledger.map((row) =>
                    detailPartCard({
                      color: LEDGER_COLORS[row.category] ?? '#64748b',
                      label: row.category,
                      value: fmtBytes(row.bytes),
                      sub: fmtPct(row.pctOfTotal),
                      detail: `Δ over window: ${fmtDeltaBytes(row.deltaBytes)}`,
                    }),
                  ),
                ),
            ],
          ),

        // Panel 2: Reclaim, Swap, PSI & LMKD Forensics (auto-open if active, quiet 1-line otherwise)
        r &&
          collapsiblePanel(
            {
              title: 'Reclaim, Swap, PSI & LMKD Forensics',
              icon: 'compress',
              color: (r.lmkdKills ?? 0) > 0 ? '#dc2626' : '#d97706',
              metric:
                r.lmkdKills > 0
                  ? `${r.lmkdKills} LMK kills`
                  : `PSI ${fmtPct(r.psiMemSomePct)}`,
              metricSub: `${fmtCount(r.directReclaimEvents)} direct reclaim · ${fmtCount(r.pswpinDelta)} swap-in`,
              badge:
                r.lmkdKills > 0
                  ? {label: 'LMK Active', tone: 'bad'}
                  : pressureActive
                    ? {label: 'Pressure', tone: 'warn'}
                    : {label: 'Quiet', tone: 'ok'},
              summary: pressureActive
                ? `PSI some ${fmtPct(r.psiMemSomePct)} / full ${fmtPct(r.psiMemFullPct)}, ${fmtCount(r.pgmajfaultDelta)} major faults, ${fmtCount(r.pswpinDelta)} swap-in / ${fmtCount(r.pswpoutDelta)} swap-out.`
                : '0 LMKD kills, 0 direct reclaim stalls — click to inspect raw vmstat counters & PSI trends.',
              quiet: !pressureActive,
              open: isOpen('reclaim', pressureActive),
              onToggle: () => togglePanel('reclaim', pressureActive),
            },
            () => [
              m('.pf-prod-stats', [
                stat(
                  'Memory PSI',
                  fmtPct(r.psiMemSomePct),
                  `full: ${fmtPct(r.psiMemFullPct)}`,
                ),
                stat(
                  'Reclaim & Compact',
                  `${fmtCount(r.directReclaimEvents)} direct`,
                  `kswapd: ${fmtCount(r.kswapdWakes)} · compact stalls: ${fmtCount(r.compactStallDelta)}`,
                ),
                stat(
                  'Paging & Swap',
                  `${fmtCount(r.pswpinDelta)} in / ${fmtCount(r.pswpoutDelta)} out`,
                  `major faults: ${fmtCount(r.pgmajfaultDelta)}`,
                ),
                stat(
                  'Active vs Inactive',
                  `${(r.activeFileMib ?? 0).toFixed(0)}M file`,
                  `anon active/inactive: ${(r.activeAnonMib ?? 0).toFixed(0)}M / ${(r.inactiveAnonMib ?? 0).toFixed(0)}M`,
                ),
              ]),
              d.pressureSeries.length >= 2 &&
                m('.pf-sysmon-chart-row', [
                  chartCard(
                    'Memory PSI over time & LMK kills',
                    memoryPsiChart(d.pressureSeries, 185, d.lmkd),
                  ),
                  chartCard(
                    'Paging & reclaim rates',
                    memoryPagingChart(d.pressureSeries, 185),
                  ),
                ]),
              d.lmkd.length > 0 &&
                simpleGrid(
                  lmkdSchema,
                  d.lmkd.map((k) => ({
                    time_s: k.relS,
                    process: k.processName,
                    pid: k.pid,
                    oom_adj: k.oomScoreAdj,
                    rss_mib: k.rssMib,
                  })),
                ),
            ],
          ),

        // Panel 3: Memory by Android Privilege Tier (OOM Buckets) — collapsed by default
        apps.buckets.length > 0 &&
          collapsiblePanel(
            {
              title: 'Memory by Android Privilege Tier (OOM Buckets)',
              icon: 'apps',
              color: '#16a34a',
              metric:
                oomTotalMib >= 1024
                  ? `${(oomTotalMib / 1024).toFixed(1)} GiB`
                  : `${oomTotalMib.toFixed(0)} MiB`,
              metricSub: topOomBucket
                ? `largest: ${oomBucketByIdx(topOomBucket.bucketIdx)?.short ?? topOomBucket.bucketName} (${topOomBucket.anonSwapMib.toFixed(0)} MiB)`
                : undefined,
              summary: apps.buckets
                .slice(0, 4)
                .map(
                  (b) =>
                    `${oomBucketByIdx(b.bucketIdx)?.short ?? b.bucketName} ${b.anonSwapMib.toFixed(0)}M (${b.procCount})`,
                )
                .join(' · '),
              quiet: true,
              open: isOpen('oom', false),
              onToggle: () => togglePanel('oom', false),
              actions: [
                {
                  label: 'Open Apps & States',
                  onclick: () => sysmonState.openSnapSubtab(snap, 'apps'),
                },
              ],
            },
            () => [
              proportionBar(
                apps.buckets.map((b) => ({
                  label: oomBucketByIdx(b.bucketIdx)?.short ?? b.bucketName,
                  value: b.anonSwapMib,
                  display: `${b.anonSwapMib.toFixed(0)} MiB · ${b.procCount} procs`,
                  color: oomBucketByIdx(b.bucketIdx)?.color,
                })),
              ),
              apps.oomSeries.length >= 2 &&
                chartCard(
                  'Anon + Swap by OOM bucket over time',
                  oomMemoryStackedChart(apps.oomSeries, 'anon_swap', undefined, 210),
                ),
            ],
          ),

        // Panel 4: Top Memory Holders & Growers (expandable ribbon rows + full table disclosure)
        d.topProcs.length > 0 &&
          collapsiblePanel(
            {
              title: 'Top Memory Holders & Growers',
              icon: 'bar_chart',
              color: '#7c3aed',
              metric: topApp ? `${topApp.anonSwapMib.toFixed(0)} MiB` : `${d.topProcs.length} procs`,
              metricSub: topApp ? `largest: ${topApp.processName}` : undefined,
              summary:
                'Ranked by Anon + Swap + DMA-BUF. Expand any row for component breakdown and direct Process / Memscope handoff.',
              open: isOpen('holders', true),
              onToggle: () => togglePanel('holders', true),
            },
            () => [
              ribbonLegend(
                HOLDER_LEGEND,
                'Shared MiB scale · click any process to inspect composition or open in Memscope',
              ),
              m(
                '.pf-prod-exp',
                topHolders.map((pr) => {
                  const open = expandedHolders.has(pr.upid);
                  const bucket = oomBucketFor(pr.oomScoreAdj);
                  return expandableRow(
                    {
                      key: `holder-${pr.upid}`,
                      open,
                      onToggle: () => {
                        if (open) expandedHolders.delete(pr.upid);
                        else expandedHolders.add(pr.upid);
                      },
                      title: m('code', pr.processName),
                      subtitle: `PID ${pr.pid} · Anon ${pr.anonMib.toFixed(0)}M · Swap ${pr.swapMib.toFixed(0)}M${pr.dmabufMib > 0.1 ? ` · DMA ${pr.dmabufMib.toFixed(0)}M` : ''}`,
                      primaryStat: `${pr.anonSwapMib.toFixed(0)} MiB`,
                      secondaryStat:
                        pr.deltaMib !== 0
                          ? fmtDeltaBytes(pr.deltaMib * 1024 * 1024)
                          : undefined,
                      chips: [
                        bucket && colorChip(bucket.short, bucket.color),
                        pr.deltaMib >= 16 &&
                          colorChip(`+${pr.deltaMib.toFixed(0)}M`, '#dc2626'),
                      ],
                      segments: [
                        {label: 'Anon', value: pr.anonMib, display: `${pr.anonMib.toFixed(0)}M`, color: '#7c3aed'},
                        {label: 'Swap', value: pr.swapMib, display: `${pr.swapMib.toFixed(0)}M`, color: '#f59e0b'},
                        {label: 'File', value: pr.fileMib, display: `${pr.fileMib.toFixed(0)}M`, color: '#0891b2'},
                        {label: 'DMA-BUF', value: pr.dmabufMib, display: `${pr.dmabufMib.toFixed(0)}M`, color: '#ec4899'},
                      ],
                      maxTotal: maxHolderMib,
                      accentColor: '#7c3aed',
                    },
                    () => [
                      m('.pf-prod-exp__parts', [
                        detailPartCard({
                          color: '#7c3aed',
                          label: 'Anon RSS',
                          value: `${pr.anonMib.toFixed(1)} MiB`,
                          sub: fmtDeltaBytes(pr.deltaMib * 1024 * 1024),
                          detail: 'Private heap, stacks, and dirty anonymous pages.',
                        }),
                        detailPartCard({
                          color: '#f59e0b',
                          label: 'Swap (ZRAM)',
                          value: `${pr.swapMib.toFixed(1)} MiB`,
                          detail: 'Anonymous pages compressed in ZRAM swap.',
                        }),
                        detailPartCard({
                          color: '#0891b2',
                          label: 'File RSS',
                          value: `${pr.fileMib.toFixed(1)} MiB`,
                          detail: 'Code (.so/.odex) and memory-mapped files.',
                        }),
                        detailPartCard({
                          color: '#ec4899',
                          label: 'DMA-BUF',
                          value: `${pr.dmabufMib.toFixed(1)} MiB`,
                          detail: 'Shared graphics, camera, and hardware buffers.',
                        }),
                      ]),
                      m('.pf-prod-exp__actions', [
                        m(Button, {
                          label: `Open ${pr.processName} tab`,
                          rightIcon: 'arrow_forward',
                          variant: ButtonVariant.Outlined,
                          compact: true,
                          onclick: () =>
                            sysmonState.openSnapSubtab(snap, 'process', pr.processName),
                        }),
                        m(Button, {
                          label: `Inspect inside ${pr.processName} in Memscope`,
                          icon: 'open_in_new',
                          variant: ButtonVariant.Minimal,
                          compact: true,
                          onclick: () =>
                            void sysmonState.openInMemscope(pr.processName),
                        }),
                      ]),
                    ],
                  );
                }),
              ),
              disclosureMore(
                {
                  open: showFullTable,
                  label: `Show full memory table (${d.topProcs.length} processes)`,
                  openLabel: 'Hide full memory table',
                  onToggle: () => {
                    showFullTable = !showFullTable;
                  },
                },
                () =>
                  simpleGrid(
                    procSchema,
                    d.topProcs.map((pr) => ({
                      process: pr.processName,
                      anon_mib: pr.anonMib,
                      swap_mib: pr.swapMib,
                      file_mib: pr.fileMib,
                      dmabuf_mib: pr.dmabufMib,
                      delta_mib: pr.deltaMib,
                      oom_adj: pr.oomScoreAdj,
                      memscope: pr.processName,
                    })),
                  ),
              ),
            ],
          ),

        limitCallout({
          badge: 'Memscope',
          title: topApp
            ? `Inspect inside \`${topApp.processName}\` (Java heap, native allocations, smaps)`
            : 'Inspect inside a single process (Java heap, native allocations, smaps)',
          body: 'Open Memscope to capture Java heap dumps, native heap profiles, and smaps breakdowns for one process.',
          actionLabel: topApp
            ? `Open ${topApp.processName} in Memscope`
            : 'Open Memscope',
          onAction: () => void sysmonState.openInMemscope(topApp?.processName),
        }),
      ]);
    },
    onremove() {
      memo.dispose();
    },
  };
}
