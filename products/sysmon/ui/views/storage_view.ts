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
import {fmtCount, fmtMs, fmtPct, fmtRateMb} from '../../../common/ui/kit/format';
import {colorChip} from '../../../common/ui/kit/sparkline';
import {
  chartCard,
  storageDirtyChart,
  storagePsiChart,
  storageThroughputChart,
} from '../charts';
import {loadStorageBundle, type StorageBundle} from '../queries';
import {sysmonState, type CapturedSnapshot} from '../state';
import {
  countCell,
  mbCell,
  msCell,
  processLinkCell,
  simpleGrid,
} from '../tables';

const IO_LEGEND = [
  {label: 'Block Read', color: '#0284c7'},
  {label: 'Block Write', color: '#ea580c'},
];

export function SnapshotStorageView(): m.Component<{
  readonly snap: CapturedSnapshot;
}> {
  const memo = new AsyncMemo<StorageBundle>();
  const panelToggles = new Map<string, boolean>();
  const expandedProcs = new Set<number>();
  let showFullTable = false;

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
        compute: () => loadStorageBundle(snap.engine, win, 25),
      }).data;

      if (!d) {
        return m('.pf-sysmon-wait', [
          m(Spinner, {easing: true}),
          m('span', 'Loading storage view…'),
        ]);
      }

      const s = d.summary;
      const top = d.procs[0];
      const hasPsiSeries = d.series.some(
        (r) => r.ioPsiPct > 0 || r.ioPsiFullPct > 0,
      );
      const pagingActive =
        (s?.dirtyMib ?? 0) >= 10 || (s?.swapIoMb ?? 0) >= 10;

      const procSchema: ColumnSchema = {
        process: {
          title: 'Process',
          columnType: 'text',
          cellRenderer: processLinkCell((name) =>
            sysmonState.openSnapSubtab(snap, 'process', name),
          ),
        },
        read_mb: {title: 'Read', columnType: 'quantitative', cellRenderer: mbCell},
        write_mb: {title: 'Write', columnType: 'quantitative', cellRenderer: mbCell},
        block_ops: {title: 'Block Ops', columnType: 'quantitative', cellRenderer: countCell},
        io_wait_ms: {title: 'D-state I/O Wait', columnType: 'quantitative', cellRenderer: msCell},
      };

      const topProcs = d.procs.slice(0, 12);
      const maxIoMb = Math.max(
        0.1,
        ...topProcs.map((pr) => pr.readMb + pr.writeMb),
      );

      return m('.pf-prod-landing', [
        // Executive Storage Verdict
        questionBlock(
          {
            question: 'Storage & I/O diagnosis',
            answer: s
              ? [
                  `Storage moved ${s.readMb.toFixed(1)} MB read (${fmtRateMb(s.readMbS)}) and ${s.writeMb.toFixed(1)} MB write (${fmtRateMb(s.writeMbS)}) across ${fmtCount(s.blockOps)} block requests, stalling threads for ${fmtMs(s.ioDstateMs)} in D-state I/O wait.`,
                  s.swapIoMb >= 10
                    ? `Swap paging accounted for ${s.swapIoMb.toFixed(1)} MB of total paging activity (file pgpgin ${s.pgpginMb.toFixed(1)} MB / pgpgout ${s.pgpgoutMb.toFixed(1)} MB).`
                    : undefined,
                ]
              : 'No storage counters or block events recorded in this snapshot.',
            answerIntent:
              (s?.ioPsiSomePct ?? 0) >= 15 || (s?.ioDstateMs ?? 0) >= 3000
                ? Intent.Danger
                : (s?.ioPsiSomePct ?? 0) >= 3 || (s?.ioDstateMs ?? 0) >= 500
                  ? Intent.Warning
                  : Intent.Primary,
            actions:
              s && s.ioDstateMs > 0
                ? [
                    {
                      label: 'Explain D-state I/O waits',
                      onclick: () =>
                        sysmonState.openSnapSubtab(snap, 'wait', 'io_wait'),
                    },
                  ]
                : undefined,
          },
          s &&
            m('.pf-prod-stats', [
              stat(
                'Read / Write',
                `${s.readMb.toFixed(1)} / ${s.writeMb.toFixed(1)} MB`,
                `${fmtRateMb(s.readMbS)} R · ${fmtRateMb(s.writeMbS)} W`,
              ),
              stat(
                'IO PSI (some)',
                fmtPct(s.ioPsiSomePct),
                `full: ${fmtPct(s.ioPsiFullPct)}`,
                (s.ioPsiSomePct ?? 0) >= 15
                  ? 'bad'
                  : (s.ioPsiSomePct ?? 0) >= 3
                    ? 'warn'
                    : undefined,
              ),
              stat(
                'Thread I/O wait',
                fmtMs(s.ioDstateMs),
                `${fmtCount(s.blockOps)} block ops`,
                s.ioDstateMs >= 3000
                  ? 'bad'
                  : s.ioDstateMs >= 500
                    ? 'warn'
                    : undefined,
              ),
              stat(
                'Dirty / Swap I/O',
                `${s.dirtyMib.toFixed(1)} MiB dirty`,
                `writeback ${s.writebackMib.toFixed(1)} MiB · swap ${s.swapIoMb.toFixed(1)} MB`,
              ),
            ]),
        ),

        // Panel 1: Throughput & IO PSI Over Time
        d.series.length > 1 &&
          collapsiblePanel(
            {
              title: 'Throughput & IO Pressure Over Time',
              icon: 'hard_drive',
              color: '#0284c7',
              metric: s ? `${(s.readMb + s.writeMb).toFixed(1)} MB` : undefined,
              metricSub: s ? `${fmtRateMb(s.readMbS)} R · ${fmtRateMb(s.writeMbS)} W` : undefined,
              summary:
                'Block read/write throughput paired with Linux I/O Pressure Stall Information (PSI).',
              open: isOpen('throughput', true),
              onToggle: () => togglePanel('throughput', true),
            },
            () => [
              m('.pf-sysmon-chart-row', [
                chartCard(
                  'Read & write throughput',
                  storageThroughputChart(d.series, 200),
                ),
                hasPsiSeries
                  ? chartCard('I/O PSI over time', storagePsiChart(d.series, 200))
                  : chartCard(
                      'Dirty & writeback page cache',
                      storageDirtyChart(d.series, 200),
                    ),
              ]),
            ],
          ),

        // Panel 2: Page Cache, Dirty Writeback & Swap Paging
        s &&
          collapsiblePanel(
            {
              title: 'Page Cache, Dirty Writeback & Swap Paging',
              icon: 'sync_alt',
              color: '#f59e0b',
              metric: `${s.dirtyMib.toFixed(1)} MiB dirty`,
              metricSub: `swap ${s.swapIoMb.toFixed(1)} MB`,
              badge: pagingActive
                ? {label: 'Active', tone: 'warn'}
                : {label: 'Quiet', tone: 'ok'},
              summary: `File paging: ${s.pgpginMb.toFixed(1)} MB in / ${s.pgpgoutMb.toFixed(1)} MB out · Swap paging: ${s.swapIoMb.toFixed(1)} MB · Writeback: ${s.writebackMib.toFixed(1)} MiB.`,
              quiet: !pagingActive,
              open: isOpen('paging', pagingActive),
              onToggle: () => togglePanel('paging', pagingActive),
            },
            () => [
              s.pgpginMb + s.pgpgoutMb + s.swapIoMb > 0 &&
                proportionBar([
                  {
                    label: 'File page-in (pgpgin)',
                    value: s.pgpginMb,
                    display: `${s.pgpginMb.toFixed(1)} MB`,
                    color: '#0284c7',
                  },
                  {
                    label: 'File page-out (pgpgout)',
                    value: s.pgpgoutMb,
                    display: `${s.pgpgoutMb.toFixed(1)} MB`,
                    color: '#ea580c',
                  },
                  {
                    label: 'Swap I/O',
                    value: s.swapIoMb,
                    display: `${s.swapIoMb.toFixed(1)} MB`,
                    color: '#f59e0b',
                  },
                ]),
              d.series.length > 1 &&
                chartCard(
                  'Dirty & writeback page cache over time',
                  storageDirtyChart(d.series, 195),
                ),
            ],
          ),

        // Panel 3: Per-Process Block I/O & D-State Drivers (expandable ribbon rows)
        d.procs.length > 0 &&
          collapsiblePanel(
            {
              title: 'Per-Process Block I/O & D-State Drivers',
              icon: 'dns',
              color: '#0891b2',
              metric: top
                ? top.totalIoMb > 0
                  ? `${top.totalIoMb.toFixed(1)} MB`
                  : fmtMs(top.ioWaitMs)
                : `${d.procs.length} procs`,
              metricSub: top ? `top: ${top.processName}` : undefined,
              summary:
                'Expand any process for Read/Write/Block-Ops/D-state breakdown, or open the Process tab or D-state Wait Explainer.',
              open: isOpen('drivers', true),
              onToggle: () => togglePanel('drivers', true),
            },
            () => [
              ribbonLegend(
                IO_LEGEND,
                'Shared MB scale · click any process for block op and D-state wait details',
              ),
              m(
                '.pf-prod-exp',
                topProcs.map((pr) => {
                  const open = expandedProcs.has(pr.upid);
                  return expandableRow(
                    {
                      key: `io-${pr.upid}`,
                      open,
                      onToggle: () => {
                        if (open) expandedProcs.delete(pr.upid);
                        else expandedProcs.add(pr.upid);
                      },
                      title: m('code', pr.processName),
                      subtitle: `${fmtCount(pr.blockOps)} block ops · Read ${pr.readMb.toFixed(1)} MB · Write ${pr.writeMb.toFixed(1)} MB`,
                      primaryStat:
                        pr.totalIoMb > 0
                          ? `${pr.totalIoMb.toFixed(1)} MB`
                          : fmtMs(pr.ioWaitMs),
                      secondaryStat:
                        pr.totalIoMb > 0 && pr.ioWaitMs > 0
                          ? `${fmtMs(pr.ioWaitMs)} wait`
                          : undefined,
                      chips: [
                        pr.ioWaitMs >= 100 &&
                          colorChip(
                            `${fmtMs(pr.ioWaitMs)} D-state`,
                            pr.ioWaitMs >= 1000 ? '#dc2626' : '#d97706',
                          ),
                      ],
                      segments: [
                        {label: 'Read', value: pr.readMb, display: `${pr.readMb.toFixed(1)} MB`, color: '#0284c7'},
                        {label: 'Write', value: pr.writeMb, display: `${pr.writeMb.toFixed(1)} MB`, color: '#ea580c'},
                      ],
                      maxTotal: maxIoMb,
                      accentColor: '#0284c7',
                    },
                    () => [
                      m('.pf-prod-exp__parts', [
                        detailPartCard({
                          color: '#0284c7',
                          label: 'Block Read',
                          value: `${pr.readMb.toFixed(2)} MB`,
                        }),
                        detailPartCard({
                          color: '#ea580c',
                          label: 'Block Write',
                          value: `${pr.writeMb.toFixed(2)} MB`,
                        }),
                        detailPartCard({
                          color: '#64748b',
                          label: 'Block Ops',
                          value: fmtCount(pr.blockOps),
                          detail:
                            pr.blockOps > 0
                              ? `Avg ${(((pr.readMb + pr.writeMb) * 1024) / pr.blockOps).toFixed(1)} KiB / op`
                              : undefined,
                        }),
                        detailPartCard({
                          color: '#dc2626',
                          label: 'D-state I/O Wait',
                          value: fmtMs(pr.ioWaitMs),
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
                        pr.ioWaitMs > 0 &&
                          m(Button, {
                            label: 'Explain D-state I/O waits',
                            rightIcon: 'arrow_forward',
                            variant: ButtonVariant.Minimal,
                            compact: true,
                            onclick: () =>
                              sysmonState.openSnapSubtab(snap, 'wait', 'io_wait'),
                          }),
                      ]),
                    ],
                  );
                }),
              ),
              disclosureMore(
                {
                  open: showFullTable,
                  label: `Show full storage process table (${d.procs.length} processes)`,
                  openLabel: 'Hide full storage process table',
                  onToggle: () => {
                    showFullTable = !showFullTable;
                  },
                },
                () =>
                  simpleGrid(
                    procSchema,
                    d.procs.map((pr) => ({
                      process: pr.processName,
                      read_mb: pr.readMb,
                      write_mb: pr.writeMb,
                      block_ops: pr.blockOps,
                      io_wait_ms: pr.ioWaitMs,
                    })),
                  ),
              ),
            ],
          ),

        limitCallout({
          badge: 'Deep Storage Analysis',
          title: 'Need per-file page cache, readahead, or block-layer latency?',
          body: 'Sysmon covers whole-device throughput, IO pressure, and per-process volume. File-level caching, readahead, and storage-stack latency require a dedicated I/O profiler.',
        }),
      ]);
    },
    onremove() {
      memo.dispose();
    },
  };
}
