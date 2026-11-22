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
import {Button, ButtonVariant} from '../../../../ui/src/widgets/button';
import {Intent} from '../../../../ui/src/widgets/common';
import {RadioGroup} from '../../../../ui/src/widgets/radio_group';
import {Spinner} from '../../../../ui/src/widgets/spinner';
import {
  areaCard,
  detailPartCard,
  disclosureMore,
  expandableRow,
  questionBlock,
  ribbonLegend,
  stat,
  type EvidenceRow,
  type RibbonSegment,
} from '../../../common/ui/kit/blocks';
import {
  pressureHeatmap,
  proportionBar,
  type PressureLane,
} from '../../../common/ui/kit/charts';
import {
  fmtBytes,
  fmtCount,
  fmtDeltaBytes,
  fmtMs,
  fmtPct,
  fmtPowerMw,
  fmtTempC,
} from '../../../common/ui/kit/format';
import {
  colorChip,
  formatAgeShort,
  oomBucketByIdx,
  oomBucketFor,
} from '../../../common/ui/kit/sparkline';
import {
  loadAppsBundle,
  loadCpuBundle,
  loadMemoryBundle,
  loadPressureBuckets,
  loadProcessDetailBundle,
  loadPulse,
  loadStorageBundle,
  loadTaskManager,
  loadThermalBundle,
  type AppsBundle,
  type CpuBundle,
  type MemoryBundle,
  type PressureBucketRow,
  type ProcessDetailBundle,
  type PulseRow,
  type StorageBundle,
  type ThermalBundle,
  type TopProcessRow,
  type WindowArgs,
} from '../queries';
import {sysmonState, type CapturedSnapshot} from '../state';
import {
  codeCell,
  deltaMibCell,
  mbCell,
  mibCell,
  msCell,
  powerMwCell,
  processLinkCell,
  simpleGrid,
  tempCell,
} from '../tables';
import {TaskManagerTable} from '../task_manager';
import {renderEventFeed} from './apps_view';

interface StartData {
  readonly pulse?: PulseRow;
  readonly buckets: PressureBucketRow[];
  readonly topProcs: TopProcessRow[];
  readonly mem: MemoryBundle;
  readonly cpu: CpuBundle;
  readonly storage: StorageBundle;
  readonly apps: AppsBundle;
  readonly thermal: ThermalBundle;
}

interface ScoredArea {
  readonly id: 'memory' | 'cpu' | 'storage' | 'thermal' | 'apps';
  readonly score: number;
  readonly quiet: boolean;
  readonly render: () => m.Child;
}

type CulpritSort = 'cpu' | 'memory' | 'contention';

const THREAD_STATE_LEGEND = [
  {label: 'Running', color: '#16a34a'},
  {label: 'Runnable (CPU wait)', color: '#dc2626'},
  {label: 'Disk I/O wait', color: '#0284c7'},
];

const MEM_LEGEND = [
  {label: 'Anon', color: '#7c3aed'},
  {label: 'Swap', color: '#f59e0b'},
  {label: 'File RSS', color: '#0891b2'},
  {label: 'DMA-BUF', color: '#ec4899'},
];

// Lazy inline forensic detail when a process culprit row is expanded on Start
// (modeled on lat/slowest.ts inline expansion).
class CulpritInlineDetail
  implements
    m.ClassComponent<{
      readonly snap: CapturedSnapshot;
      readonly proc: TopProcessRow;
      readonly win?: WindowArgs;
    }>
{
  private readonly memo = new AsyncMemo<ProcessDetailBundle>();

  onremove() {
    this.memo.dispose();
  }

  view({
    attrs: {snap, proc, win},
  }: m.CVnode<{
    readonly snap: CapturedSnapshot;
    readonly proc: TopProcessRow;
    readonly win?: WindowArgs;
  }>) {
    const detail = this.memo.use({
      key: `${snap.id}:${proc.upid}:${win?.startTs?.toString() ?? 'all'}:${win?.endTs?.toString() ?? 'all'}`,
      compute: () => loadProcessDetailBundle(snap.engine, proc.upid, win, true),
    }).data;

    const bucket = oomBucketFor(proc.oomScoreAdj);
    const delayRatio =
      proc.cpuMs >= 0.5 ? proc.runnableMs / proc.cpuMs : undefined;

    return [
      m('.pf-prod-exp__parts', [
        detailPartCard({
          color: '#16a34a',
          label: 'CPU & Contention',
          value: fmtPct(proc.cpuPct),
          sub: delayRatio !== undefined ? `${delayRatio.toFixed(2)}× delay` : undefined,
          detail: `${fmtMs(proc.cpuMs)} running · ${fmtMs(proc.runnableMs)} runnable · ${fmtMs(proc.ioWaitMs)} I/O wait`,
        }),
        detailPartCard({
          color: '#7c3aed',
          label: 'Anon + Swap',
          value: `${proc.anonSwapMib.toFixed(1)} MiB`,
          sub: fmtDeltaBytes(proc.memDeltaMib * 1024 * 1024),
          detail: `Anon ${proc.anonMib.toFixed(1)} MiB · Swap ${proc.swapMib.toFixed(1)} MiB`,
        }),
        detailPartCard({
          color: '#0891b2',
          label: 'File & Shared',
          value: `${(proc.fileMib + proc.dmabufMib).toFixed(1)} MiB`,
          sub: proc.dmabufMib > 0.1 ? `${proc.dmabufMib.toFixed(1)}M DMA-BUF` : undefined,
          detail: `File RSS ${proc.fileMib.toFixed(1)} MiB · DMA-BUF ${proc.dmabufMib.toFixed(1)} MiB`,
        }),
        detailPartCard({
          color: bucket?.color ?? '#64748b',
          label: 'Android State',
          value: proc.isTop ? 'Top App' : bucket?.short ?? '—',
          sub: `PID ${proc.pid}`,
          detail: `oom_score_adj ${proc.oomScoreAdj ?? '—'} · Age ${formatAgeShort(proc.ageS)}${proc.frozen ? ' · Frozen' : ''}`,
        }),
      ]),
      detail === undefined
        ? m('.pf-sysmon-wait', {style: {padding: '8px 0'}}, [
            m(Spinner, {easing: true}),
            m('span', `Loading thread breakdown for ${proc.processName}…`),
          ])
        : detail.threads.length > 0 &&
          simpleGrid(
            {
              thread: {title: 'Top Threads', columnType: 'text', cellRenderer: codeCell},
              running_ms: {title: 'Running', columnType: 'quantitative', cellRenderer: msCell},
              runnable_ms: {title: 'Runnable', columnType: 'quantitative', cellRenderer: msCell},
              delay: {title: 'CPU Delay', columnType: 'text'},
              io_wait_ms: {title: 'I/O Wait', columnType: 'quantitative', cellRenderer: msCell},
              blocked: {title: 'Top Blocked Kernel Fn', columnType: 'text', cellRenderer: codeCell},
            },
            detail.threads.slice(0, 5).map((t) => ({
              thread: `${t.threadName} (${t.tid})`,
              running_ms: t.runningMs,
              runnable_ms: t.runnableMs,
              delay: t.runningMs >= 0.5 ? `${t.cpuDelayRatio.toFixed(2)}×` : '—',
              io_wait_ms: t.ioWaitMs,
              blocked: t.topBlockedFn ?? '—',
            })),
          ),
      m('.pf-prod-exp__actions', [
        m(Button, {
          label: `Open ${proc.processName} tab`,
          rightIcon: 'arrow_forward',
          variant: ButtonVariant.Outlined,
          compact: true,
          onclick: () =>
            sysmonState.openSnapSubtab(snap, 'process', proc.processName),
        }),
        m(Button, {
          label: 'Open in Memscope',
          icon: 'open_in_new',
          variant: ButtonVariant.Minimal,
          compact: true,
          onclick: () => void sysmonState.openInMemscope(proc.processName),
        }),
      ]),
    ];
  }
}

// Start view inside a Snapshot (modeled on lat/scope_overview.ts + slowest.ts):
// deep forensic entry point distinct from Live Overview:
// 1) Plain-English diagnosis + brushable 4-lane pressure timeline
// 2) Ranked subsystem cards (with rich inline slide-outs; quiet areas 1-line)
// 3) Expandable forensic culprit rows with shared-scale ribbons & lazy drill-down
// 4) Full searchable Task Manager tucked behind a disclosure toggle.
export function SnapshotStartView(): m.Component<{
  readonly snap: CapturedSnapshot;
}> {
  const memo = new AsyncMemo<StartData>();
  const openedAreas = new Set<string>();
  const expandedCulprits = new Set<number>();
  let culpritSort: CulpritSort = 'cpu';
  let showFullTable = false;

  return {
    view({attrs: {snap}}) {
      const win = sysmonState.getWindowArgs(snap);
      const d = memo.use({
        key: {
          id: snap.id,
          s: win?.startTs?.toString() ?? 'all',
          e: win?.endTs?.toString() ?? 'all',
        },
        compute: async () => {
          const [pulse, buckets, topProcs, mem, cpu, storage, apps, thermal] =
            await Promise.all([
              loadPulse(snap.engine, win),
              loadPressureBuckets(snap.engine, undefined, 40),
              loadTaskManager(snap.engine, win, 80, 15),
              loadMemoryBundle(snap.engine, win, 8),
              loadCpuBundle(snap.engine, win, 8),
              loadStorageBundle(snap.engine, win, 8),
              loadAppsBundle(snap.engine, win, 10),
              loadThermalBundle(snap.engine, win, 8),
            ]);
          return {pulse, buckets, topProcs, mem, cpu, storage, apps, thermal};
        },
      }).data;

      if (!d || !d.pulse) {
        return m('.pf-sysmon-wait', [
          m(Spinner, {easing: true}),
          m('span', 'Analyzing snapshot…'),
        ]);
      }

      const p = d.pulse;
      const areas = buildRankedAreas(snap, d, openedAreas);
      const verdict = synthesizeVerdict(p, d);

      const lanes: PressureLane[] = [
        {
          id: 'cpu',
          label: 'CPU',
          buckets: d.buckets.map((b) => b.cpuPressure),
        },
        {
          id: 'memory',
          label: 'Memory',
          buckets: d.buckets.map((b) => b.memPressure),
        },
        {
          id: 'storage',
          label: 'Storage',
          buckets: d.buckets.map((b) => b.ioPressure),
        },
        {
          id: 'thermal',
          label: 'Thermal',
          buckets: d.buckets.map((b) => b.thermalPressure),
        },
      ];

      const memAvailPct =
        p.memTotalBytes && p.memAvailMinBytes !== null
          ? (100 * p.memAvailMinBytes) / p.memTotalBytes
          : null;

      // Pick top 10 forensic culprits for the selected lens.
      const culprits = [...d.topProcs]
        .sort((a, b) => {
          if (culpritSort === 'memory') {
            return (
              b.anonSwapMib +
              b.dmabufMib -
              (a.anonSwapMib + a.dmabufMib) ||
              Math.abs(b.memDeltaMib) - Math.abs(a.memDeltaMib)
            );
          }
          if (culpritSort === 'contention') {
            return (
              b.runnableMs +
              b.ioWaitMs -
              (a.runnableMs + a.ioWaitMs) ||
              b.cpuMs - a.cpuMs
            );
          }
          return (
            b.cpuMs +
            b.runnableMs +
            b.ioWaitMs -
            (a.cpuMs + a.runnableMs + a.ioWaitMs)
          );
        })
        .slice(0, 10);

      const maxRibbon =
        culpritSort === 'memory'
          ? Math.max(
              1,
              ...culprits.map(
                (c) => c.anonMib + c.swapMib + c.fileMib + c.dmabufMib,
              ),
            )
          : Math.max(
              1,
              ...culprits.map((c) => c.cpuMs + c.runnableMs + c.ioWaitMs),
            );

      const toggleCulprit = (upid: number) => {
        if (expandedCulprits.has(upid)) expandedCulprits.delete(upid);
        else expandedCulprits.add(upid);
      };

      return m('.pf-prod-landing', [
        // Block 1: Executive forensic diagnosis + KPI strip + brushable heatmap
        questionBlock(
          {
            question: snap.brush
              ? 'What was under pressure in this brushed slice?'
              : 'Forensic diagnosis: what was under pressure?',
            answer: verdict.points,
            answerIntent: verdict.intent,
            actions: [
              {
                label: 'Apps & States',
                icon: 'apps',
                onclick: () => sysmonState.openSnapSubtab(snap, 'apps'),
              },
              ...(snap.brush
                ? [
                    {
                      label: 'Clear time brush',
                      icon: 'filter_alt_off',
                      onclick: () => {
                        snap.brush = undefined;
                      },
                    },
                  ]
                : []),
            ],
          },
          [
            m('.pf-prod-stats', [
              stat(
                'CPU busy',
                fmtPct(p.cpuBusyPct),
                p.cpuPsiSomePct !== null
                  ? `CPU PSI ${fmtPct(p.cpuPsiSomePct)}`
                  : `${p.cpuCount} cores`,
                (p.cpuBusyPct ?? 0) >= 75 || (p.cpuPsiSomePct ?? 0) >= 15
                  ? 'bad'
                  : (p.cpuBusyPct ?? 0) >= 45 || (p.cpuPsiSomePct ?? 0) >= 5
                    ? 'warn'
                    : undefined,
              ),
              stat(
                'Min RAM available',
                p.hasMeminfo ? fmtBytes(p.memAvailMinBytes) : '—',
                p.hasMeminfo
                  ? `of ${fmtBytes(p.memTotalBytes)}${p.lmkdKills > 0 ? ` · ${p.lmkdKills} LMKD kills` : ''}`
                  : 'No meminfo in trace',
                p.lmkdKills > 0 || (memAvailPct !== null && memAvailPct < 15)
                  ? 'bad'
                  : (p.memPsiSomePct ?? 0) >= 3 ||
                      (memAvailPct !== null && memAvailPct < 25)
                    ? 'warn'
                    : undefined,
              ),
              stat(
                'Storage I/O',
                `${(p.ioReadMb + p.ioWriteMb).toFixed(1)} MB`,
                p.ioDstateMs > 0
                  ? `${fmtMs(p.ioDstateMs)} D-state wait${p.ioPsiSomePct !== null ? ` · IO PSI ${fmtPct(p.ioPsiSomePct)}` : ''}`
                  : p.ioPsiSomePct !== null
                    ? `IO PSI ${fmtPct(p.ioPsiSomePct)}`
                    : 'Quiet',
                (p.ioPsiSomePct ?? 0) >= 15 || p.ioDstateMs >= 3000
                  ? 'bad'
                  : (p.ioPsiSomePct ?? 0) >= 3 || p.ioDstateMs >= 500
                    ? 'warn'
                    : undefined,
              ),
              stat(
                'Thermal & Power',
                p.hasThermal
                  ? fmtTempC(p.peakTempC)
                  : p.hasPower
                    ? fmtPowerMw(p.avgPowerMw)
                    : 'Quiet / N/A',
                p.throttleEvents > 0
                  ? `${p.throttleEvents} freq-cap events`
                  : p.hasPower && p.hasThermal
                    ? fmtPowerMw(p.avgPowerMw)
                    : 'No throttling detected',
                p.throttleEvents > 0 || (p.peakTempC ?? 0) >= 48
                  ? 'warn'
                  : undefined,
              ),
            ]),
            d.buckets.length > 0 &&
              pressureHeatmap({
                lanes,
                durationS: snap.pulse?.durS ?? p.durS,
                selection: snap.brush,
                onChange: (sel) => {
                  snap.brush = sel;
                },
              }),
          ],
        ),

        // Block 2: Ranked subsystem cards (quiet areas 1-line & expandable)
        questionBlock(
          {
            question: 'Subsystem breakdown (ranked by pressure)',
            description:
              'Expand any card for inline evidence, or open its dedicated tab for full forensic charts and tables.',
          },
          m(
            '.pf-prod-sections',
            areas.map((a) => a.render()),
          ),
        ),

        // Block 3: lat/slowest-style expandable Top Resource Culprits
        culprits.length > 0 &&
          questionBlock(
            {
              question: 'Top resource culprits (click any row to inspect threads & memory in place)',
              description:
                'Bars share one scale so relative impact stands out immediately. Expand a row for its thread contention and memory split without leaving Start.',
            },
            [
              m('.pf-sysmon-oom-controls', [
                m(
                  RadioGroup,
                  {
                    intent: Intent.Primary,
                    selectedValue: culpritSort,
                    onValueChange: (v) => {
                      culpritSort = v as CulpritSort;
                    },
                  },
                  [
                    m(RadioGroup.Button, {value: 'cpu'}, 'CPU & Waits'),
                    m(RadioGroup.Button, {value: 'contention'}, 'Contention (Runnable + I/O)'),
                    m(RadioGroup.Button, {value: 'memory'}, 'Memory & Growth'),
                  ],
                ),
              ]),
              ribbonLegend(
                culpritSort === 'memory' ? MEM_LEGEND : THREAD_STATE_LEGEND,
                'Click any row to expand inline thread & memory details',
              ),
              m(
                '.pf-prod-exp',
                culprits.map((proc) => {
                  const bucket = oomBucketFor(proc.oomScoreAdj);
                  const segs: RibbonSegment[] =
                    culpritSort === 'memory'
                      ? [
                          {
                            label: 'Anon',
                            value: proc.anonMib,
                            display: `${proc.anonMib.toFixed(0)}M`,
                            color: '#7c3aed',
                          },
                          {
                            label: 'Swap',
                            value: proc.swapMib,
                            display: `${proc.swapMib.toFixed(0)}M`,
                            color: '#f59e0b',
                          },
                          {
                            label: 'File',
                            value: proc.fileMib,
                            display: `${proc.fileMib.toFixed(0)}M`,
                            color: '#0891b2',
                          },
                          {
                            label: 'DMA-BUF',
                            value: proc.dmabufMib,
                            display: `${proc.dmabufMib.toFixed(0)}M`,
                            color: '#ec4899',
                          },
                        ]
                      : [
                          {
                            label: 'Running',
                            value: proc.cpuMs,
                            display: fmtMs(proc.cpuMs),
                            color: '#16a34a',
                          },
                          {
                            label: 'Runnable',
                            value: proc.runnableMs,
                            display: fmtMs(proc.runnableMs),
                            color: '#dc2626',
                          },
                          {
                            label: 'I/O Wait',
                            value: proc.ioWaitMs,
                            display: fmtMs(proc.ioWaitMs),
                            color: '#0284c7',
                          },
                        ];

                  const primaryStat =
                    culpritSort === 'memory'
                      ? `${proc.anonSwapMib.toFixed(0)} MiB`
                      : culpritSort === 'contention'
                        ? fmtMs(proc.runnableMs + proc.ioWaitMs)
                        : fmtPct(proc.cpuPct);

                  const secondaryStat =
                    culpritSort === 'memory'
                      ? proc.memDeltaMib !== 0
                        ? fmtDeltaBytes(proc.memDeltaMib * 1024 * 1024)
                        : undefined
                      : culpritSort === 'contention'
                        ? `${fmtMs(proc.cpuMs)} run`
                        : proc.runnableMs + proc.ioWaitMs > 1
                          ? `${fmtMs(proc.runnableMs + proc.ioWaitMs)} wait`
                          : fmtMs(proc.cpuMs);

                  return expandableRow(
                    {
                      key: `culprit-${proc.upid}`,
                      open: expandedCulprits.has(proc.upid),
                      onToggle: () => toggleCulprit(proc.upid),
                      title: m('code', proc.processName),
                      subtitle: `PID ${proc.pid} · ${proc.anonSwapMib.toFixed(0)} MiB Anon+Swap · CPU ${fmtPct(proc.cpuPct)}`,
                      primaryStat,
                      secondaryStat,
                      chips: [
                        proc.isTop
                          ? colorChip('TOP', '#16a34a')
                          : bucket && colorChip(bucket.short, bucket.color),
                        proc.memDeltaMib >= 16 &&
                          colorChip(
                            `+${proc.memDeltaMib.toFixed(0)}M`,
                            '#dc2626',
                          ),
                      ],
                      segments: segs,
                      maxTotal: maxRibbon,
                      accentColor:
                        culpritSort === 'memory' ? '#7c3aed' : '#2563eb',
                    },
                    () => m(CulpritInlineDetail, {snap, proc, win}),
                  );
                }),
              ),

              // Collapsed-by-default full Task Manager table
              disclosureMore(
                {
                  open: showFullTable,
                  label: `Show full process table (${d.topProcs.length} processes)`,
                  openLabel: `Hide full process table`,
                  onToggle: () => {
                    showFullTable = !showFullTable;
                  },
                },
                () =>
                  m(TaskManagerTable, {
                    processes: d.topProcs,
                    defaultScope: 'all',
                    showSnapshotWaits: true,
                    maxRows: 80,
                    onOpenProcess: (name) =>
                      sysmonState.openSnapSubtab(snap, 'process', name),
                  }),
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

function synthesizeVerdict(
  p: PulseRow,
  d: StartData,
): {points: string[]; intent: Intent} {
  const points: string[] = [];
  let severity = 0;

  const runnableWait = d.cpu.waits.find((w) => w.category === 'runnable');
  const ioWait = d.cpu.waits.find((w) => w.category === 'io_wait');

  if (
    p.lmkdKills > 0 ||
    (p.memPsiSomePct ?? 0) >= 3 ||
    p.directReclaimEvents > 0
  ) {
    severity = Math.max(severity, p.lmkdKills > 0 ? 2 : 1);
    const topMem = d.mem.topProcs[0];
    points.push(
      `Memory pressure was active: ${p.lmkdKills > 0 ? `${p.lmkdKills} LMKD kills, ` : ''}${p.memPsiSomePct !== null ? `Memory PSI ${fmtPct(p.memPsiSomePct)}, ` : ''}min available RAM ${fmtBytes(p.memAvailMinBytes)}` +
        (topMem
          ? ` — largest holder \`${topMem.processName}\` (${topMem.anonSwapMib.toFixed(0)} MiB).`
          : '.'),
    );
  }

  if (
    (p.cpuBusyPct ?? 0) >= 40 ||
    (p.cpuPsiSomePct ?? 0) >= 5 ||
    (runnableWait && runnableWait.sharePct >= 20)
  ) {
    severity = Math.max(
      severity,
      (p.cpuBusyPct ?? 0) >= 75 || (p.cpuPsiSomePct ?? 0) >= 15 ? 2 : 1,
    );
    const topThr = d.cpu.topThreads[0];
    points.push(
      `CPU contention stood out: ${fmtPct(p.cpuBusyPct)} busy` +
        (p.cpuPsiSomePct !== null ? ` (CPU PSI ${fmtPct(p.cpuPsiSomePct)})` : '') +
        (runnableWait
          ? `, with ${fmtPct(runnableWait.sharePct)} of active thread time queued runnable (${fmtMs(runnableWait.totalMs)})`
          : '') +
        (topThr ? ` — busiest \`${topThr.processName}\`.` : '.'),
    );
  }

  if (
    (p.ioPsiSomePct ?? 0) >= 2 ||
    p.ioDstateMs >= 400 ||
    p.ioReadMb + p.ioWriteMb >= 40
  ) {
    severity = Math.max(
      severity,
      (p.ioPsiSomePct ?? 0) >= 15 || p.ioDstateMs >= 3000 ? 2 : 1,
    );
    const topIo = d.storage.procs[0];
    points.push(
      `Storage activity moved ${p.ioReadMb.toFixed(1)} MB read / ${p.ioWriteMb.toFixed(1)} MB write` +
        (ioWait || p.ioDstateMs > 0
          ? ` and stalled threads for ${fmtMs(p.ioDstateMs)} in D-state I/O wait`
          : '') +
        (topIo ? ` — top driver \`${topIo.processName}\`.` : '.'),
    );
  }

  if (p.throttleEvents > 0 || (p.peakTempC ?? 0) >= 48) {
    severity = Math.max(severity, 1);
    points.push(
      `Thermal limits engaged: peak ${fmtTempC(p.peakTempC)} with ${p.throttleEvents} cooling-device frequency updates.`,
    );
  }

  if (points.length === 0) {
    points.push(
      `No major system-wide bottleneck in this ${p.durS.toFixed(1)}s window: CPU ${fmtPct(p.cpuBusyPct)}, ` +
        (p.hasMeminfo ? `min available RAM ${fmtBytes(p.memAvailMinBytes)}, ` : '') +
        `storage ${(p.ioReadMb + p.ioWriteMb).toFixed(1)} MB.`,
    );
  }

  return {
    points,
    intent:
      severity >= 2
        ? Intent.Danger
        : severity === 1
          ? Intent.Warning
          : Intent.Primary,
  };
}

function buildRankedAreas(
  snap: CapturedSnapshot,
  d: StartData,
  opened: Set<string>,
): ScoredArea[] {
  const p = d.pulse!;
  const toggle = (id: string) => {
    if (opened.has(id)) opened.delete(id);
    else opened.add(id);
  };

  // 1. CPU score
  const runnable = d.cpu.waits.find((w) => w.category === 'runnable');
  const ioWait = d.cpu.waits.find((w) => w.category === 'io_wait');
  const cpuScore =
    (p.cpuBusyPct ?? 0) +
    (p.cpuPsiSomePct ?? 0) * 3 +
    (runnable?.sharePct ?? 0) * 1.2;
  const cpuQuiet =
    (p.cpuBusyPct ?? 0) < 25 &&
    (p.cpuPsiSomePct ?? 0) < 3 &&
    (runnable?.sharePct ?? 0) < 15;

  const cpuEv: EvidenceRow[] = [];
  if (d.cpu.waits.length > 0) {
    cpuEv.push({
      label: 'Thread time',
      total: d.cpu.waits.reduce((a, w) => a + w.totalMs, 0),
      parts: d.cpu.waits.map((w) => ({
        label: w.label,
        value: w.totalMs,
        display: fmtPct(w.sharePct),
      })),
    });
  }
  if (d.cpu.topThreads.length > 0) {
    cpuEv.push({
      label: 'Top threads',
      total: d.cpu.topThreads.reduce((a, t) => a + t.totalActiveMs, 0),
      parts: d.cpu.topThreads.slice(0, 3).map((t) => ({
        label: `${t.processName}/${t.threadName}`,
        value: t.totalActiveMs,
        display: fmtMs(t.totalActiveMs),
      })),
    });
  }

  // 2. Memory score
  const memUsedPct =
    p.memTotalBytes && p.memAvailMinBytes !== null
      ? (100 * (p.memTotalBytes - p.memAvailMinBytes)) / p.memTotalBytes
      : 0;
  const swapMb = d.storage.summary?.swapIoMb ?? 0;
  const memScore =
    p.lmkdKills * 60 +
    (p.memPsiSomePct ?? 0) * 5 +
    p.directReclaimEvents * 2 +
    Math.max(0, memUsedPct - 50) +
    Math.min(40, swapMb / 10);
  const memQuiet =
    !p.hasMeminfo && d.mem.topProcs.length === 0
      ? true
      : p.lmkdKills === 0 &&
        (p.memPsiSomePct ?? 0) < 1 &&
        p.directReclaimEvents === 0 &&
        memUsedPct < 65 &&
        swapMb < 50;

  const memEv: EvidenceRow[] = [];
  if (d.mem.ledger.length > 0) {
    memEv.push({
      label: 'RAM split',
      total: p.memTotalBytes ?? 0,
      parts: d.mem.ledger
        .filter((l) => l.category !== 'Free' && l.category !== 'Other / Reserved')
        .sort((a, b) => b.bytes - a.bytes)
        .slice(0, 3)
        .map((l) => ({
          label: l.category,
          value: l.bytes,
          display: fmtBytes(l.bytes),
        })),
    });
  }
  if (d.mem.topProcs.length > 0) {
    memEv.push({
      label: 'Top holders',
      total: d.mem.topProcs.reduce((a, pr) => a + pr.anonSwapMib, 0),
      parts: d.mem.topProcs.slice(0, 3).map((pr) => ({
        label: pr.processName,
        value: pr.anonSwapMib,
        display: `${pr.anonSwapMib.toFixed(0)} MiB`,
      })),
    });
  }

  // 3. Storage score
  const totalIo = p.ioReadMb + p.ioWriteMb;
  const storageScore =
    (p.ioPsiSomePct ?? 0) * 4 +
    Math.min(60, totalIo / 3) +
    Math.min(60, p.ioDstateMs / 80);
  const storageQuiet =
    (p.ioPsiSomePct ?? 0) < 1 && totalIo < 20 && p.ioDstateMs < 250;

  const storageEv: EvidenceRow[] = [];
  if (d.storage.procs.length > 0) {
    storageEv.push({
      label: 'Top I/O procs',
      total: d.storage.procs.reduce(
        (a, pr) => a + pr.totalIoMb + pr.ioWaitMs / 10,
        0,
      ),
      parts: d.storage.procs.slice(0, 3).map((pr) => ({
        label: pr.processName,
        value: pr.totalIoMb + pr.ioWaitMs / 10,
        display:
          pr.totalIoMb > 0
            ? `${pr.totalIoMb.toFixed(1)} MB`
            : `${fmtMs(pr.ioWaitMs)} wait`,
      })),
    });
  }

  // 4. Apps & Lifecycle score
  const topApp = d.apps.topApp;
  const lmkKills = topApp?.lmkdKills ?? p.lmkdKills;
  const appsScore = lmkKills * 50 + Math.min(35, d.apps.events.length * 2.5);
  const appsQuiet = lmkKills === 0 && d.apps.events.length < 4;

  const appsEv: EvidenceRow[] = [];
  if (d.apps.buckets.length > 0) {
    appsEv.push({
      label: 'OOM RAM',
      total: d.apps.buckets.reduce((a, b) => a + b.anonSwapMib, 0),
      parts: [...d.apps.buckets]
        .sort((a, b) => b.anonSwapMib - a.anonSwapMib)
        .slice(0, 3)
        .map((b) => ({
          label: oomBucketByIdx(b.bucketIdx)?.short ?? b.bucketName,
          value: b.anonSwapMib,
          display: `${b.anonSwapMib.toFixed(0)} MiB`,
          color: oomBucketByIdx(b.bucketIdx)?.color,
        })),
    });
  }

  // 5. Thermal & Power score
  const vsysRail = d.thermal.rails.find((r) => r.rail.includes('VSYS_ALL'));
  const subRails = d.thermal.rails.filter((r) => !r.rail.includes('VSYS_ALL'));
  const totalMw = vsysRail
    ? vsysRail.avgMw
    : subRails.reduce((a, r) => a + r.avgMw, 0);
  const thermalScore =
    p.throttleEvents * 15 + Math.max(0, ((p.peakTempC ?? 0) - 40) * 4);
  const thermalQuiet = p.throttleEvents === 0 && (p.peakTempC ?? 0) < 44;

  const maxScore = Math.max(
    1,
    cpuScore,
    memScore,
    storageScore,
    appsScore,
    thermalScore,
  );

  const list: ScoredArea[] = [
    {
      id: 'cpu',
      score: cpuScore,
      quiet: cpuQuiet,
      render: () =>
        areaCard({
          id: 'cpu',
          title: 'CPU & Scheduling',
          icon: 'speed',
          color: '#2563eb',
          headline: fmtPct(p.cpuBusyPct),
          headlineSub:
            p.cpuPsiSomePct !== null ? `PSI ${fmtPct(p.cpuPsiSomePct)}` : undefined,
          barFrac: cpuScore / maxScore,
          quiet: cpuQuiet,
          quietSummary: `${fmtPct(p.cpuBusyPct)} busy across ${p.cpuCount} cores${p.cpuPsiSomePct !== null ? `, PSI ${fmtPct(p.cpuPsiSomePct)}` : ''}`,
          finding: runnable
            ? `Active thread time split: ${fmtPct(runnable.sharePct)} waiting runnable (${fmtMs(runnable.totalMs)}), ${fmtMs(p.ioDstateMs)} in I/O wait.`
            : `Aggregate CPU utilization ${fmtPct(p.cpuBusyPct)} across ${p.cpuCount} cores.`,
          evidence: cpuEv,
          open: opened.has('cpu'),
          onToggle: () => toggle('cpu'),
          onOpenTab: () => sysmonState.openSnapSubtab(snap, 'cpu'),
          moreContent: () => [
            d.cpu.waits.length > 0 &&
              proportionBar(
                d.cpu.waits.map((w) => ({
                  label: w.label,
                  value: w.totalMs,
                  display: `${fmtMs(w.totalMs)} (${fmtPct(w.sharePct)})`,
                  color:
                    w.category === 'running'
                      ? '#16a34a'
                      : w.category === 'runnable'
                        ? '#dc2626'
                        : w.category === 'io_wait'
                          ? '#0284c7'
                          : '#d97706',
                })),
              ),
            d.cpu.topThreads.length > 0 &&
              simpleGrid(
                {
                  process: {
                    title: 'Process',
                    columnType: 'text',
                    cellRenderer: processLinkCell((name) =>
                      sysmonState.openSnapSubtab(snap, 'process', name),
                    ),
                  },
                  thread: {title: 'Thread', columnType: 'text', cellRenderer: codeCell},
                  running_ms: {title: 'Running', columnType: 'quantitative', cellRenderer: msCell},
                  runnable_ms: {title: 'Runnable', columnType: 'quantitative', cellRenderer: msCell},
                  io_wait_ms: {title: 'I/O Wait', columnType: 'quantitative', cellRenderer: msCell},
                },
                d.cpu.topThreads.slice(0, 6).map((t) => ({
                  process: t.processName,
                  thread: t.threadName,
                  running_ms: t.runningMs,
                  runnable_ms: t.runnableMs,
                  io_wait_ms: t.ioWaitMs,
                })),
              ),
            m('.pf-prod-exp__actions', [
              runnable &&
                runnable.totalMs > 0 &&
                m(Button, {
                  label: 'Explain Runnable contention',
                  rightIcon: 'arrow_forward',
                  variant: ButtonVariant.Outlined,
                  compact: true,
                  onclick: () =>
                    sysmonState.openSnapSubtab(snap, 'wait', 'runnable'),
                }),
              ioWait &&
                ioWait.totalMs > 0 &&
                m(Button, {
                  label: 'Explain Disk I/O waits',
                  rightIcon: 'arrow_forward',
                  variant: ButtonVariant.Outlined,
                  compact: true,
                  onclick: () =>
                    sysmonState.openSnapSubtab(snap, 'wait', 'io_wait'),
                }),
            ]),
          ],
        }),
    },
    {
      id: 'memory',
      score: memScore,
      quiet: memQuiet,
      render: () =>
        areaCard({
          id: 'memory',
          title: 'Memory',
          icon: 'memory',
          color: '#7c3aed',
          headline: p.hasMeminfo
            ? fmtBytes(p.memAvailMinBytes)
            : `${d.mem.topProcs.length} procs`,
          headlineSub: p.hasMeminfo
            ? `min avail of ${fmtBytes(p.memTotalBytes)}`
            : undefined,
          barFrac: memScore / maxScore,
          quiet: memQuiet,
          quietSummary: p.hasMeminfo
            ? `${fmtBytes(p.memAvailMinBytes)} available, ${p.lmkdKills} LMKD kills, PSI ${fmtPct(p.memPsiSomePct)}`
            : 'No meminfo counters in trace',
          finding:
            `Anon ${fmtBytes(p.memAnonBytes)}, File Cache ${fmtBytes(p.memCacheBytes)}, DMA-BUF+GPU ${fmtBytes(p.memDmabufGpuBytes)}` +
            (swapMb >= 10 ? `, Swap I/O ${swapMb.toFixed(0)} MB` : '') +
            (p.lmkdKills > 0 ? `, ${p.lmkdKills} LMKD kills.` : '.'),
          evidence: memEv,
          open: opened.has('memory'),
          onToggle: () => toggle('memory'),
          onOpenTab: () => sysmonState.openSnapSubtab(snap, 'memory'),
          moreContent: () => [
            d.mem.reclaim &&
              m('.pf-prod-stats', [
                stat(
                  'Memory PSI',
                  fmtPct(d.mem.reclaim.psiMemSomePct),
                  `full ${fmtPct(d.mem.reclaim.psiMemFullPct)}`,
                ),
                stat(
                  'Direct Reclaim',
                  fmtCount(d.mem.reclaim.directReclaimEvents),
                  `kswapd ${fmtCount(d.mem.reclaim.kswapdWakes)}`,
                ),
                stat(
                  'Swap Pages',
                  `${fmtCount(d.mem.reclaim.pswpinDelta)} in / ${fmtCount(d.mem.reclaim.pswpoutDelta)} out`,
                  `majfault ${fmtCount(d.mem.reclaim.pgmajfaultDelta)}`,
                ),
              ]),
            d.mem.topProcs.length > 0 &&
              simpleGrid(
                {
                  process: {
                    title: 'Top Memory Holders',
                    columnType: 'text',
                    cellRenderer: processLinkCell((name) =>
                      sysmonState.openSnapSubtab(snap, 'process', name),
                    ),
                  },
                  anon_swap_mib: {title: 'Anon + Swap', columnType: 'quantitative', cellRenderer: mibCell},
                  dmabuf_mib: {title: 'DMA-BUF', columnType: 'quantitative', cellRenderer: mibCell},
                  delta_mib: {title: 'Δ', columnType: 'quantitative', cellRenderer: deltaMibCell},
                },
                d.mem.topProcs.slice(0, 6).map((pr) => ({
                  process: pr.processName,
                  anon_swap_mib: pr.anonSwapMib,
                  dmabuf_mib: pr.dmabufMib,
                  delta_mib: pr.deltaMib,
                })),
              ),
          ],
        }),
    },
    {
      id: 'storage',
      score: storageScore,
      quiet: storageQuiet,
      render: () =>
        areaCard({
          id: 'storage',
          title: 'Storage & I/O',
          icon: 'hard_drive',
          color: '#0284c7',
          headline: `${totalIo.toFixed(1)} MB`,
          headlineSub:
            p.ioDstateMs > 0 ? `${fmtMs(p.ioDstateMs)} D-state wait` : undefined,
          barFrac: storageScore / maxScore,
          quiet: storageQuiet,
          quietSummary: `${totalIo.toFixed(1)} MB total, IO PSI ${fmtPct(p.ioPsiSomePct)}`,
          finding:
            `Read ${p.ioReadMb.toFixed(1)} MB, Write ${p.ioWriteMb.toFixed(1)} MB` +
            (p.ioPsiSomePct !== null ? `, IO PSI ${fmtPct(p.ioPsiSomePct)}` : '') +
            (p.ioDstateMs > 0 ? `, ${fmtMs(p.ioDstateMs)} thread I/O wait.` : '.'),
          evidence: storageEv,
          open: opened.has('storage'),
          onToggle: () => toggle('storage'),
          onOpenTab: () => sysmonState.openSnapSubtab(snap, 'storage'),
          moreContent: () => [
            d.storage.summary &&
              m('.pf-prod-stats', [
                stat(
                  'Read / Write',
                  `${d.storage.summary.readMb.toFixed(1)} / ${d.storage.summary.writeMb.toFixed(1)} MB`,
                  `${fmtCount(d.storage.summary.blockOps)} block ops`,
                ),
                stat(
                  'Dirty / Writeback',
                  `${d.storage.summary.dirtyMib.toFixed(1)} MiB`,
                  `writeback ${d.storage.summary.writebackMib.toFixed(1)} MiB`,
                ),
                stat(
                  'Swap Paging',
                  `${d.storage.summary.swapIoMb.toFixed(1)} MB`,
                  `file in ${d.storage.summary.pgpginMb.toFixed(1)} / out ${d.storage.summary.pgpgoutMb.toFixed(1)} MB`,
                ),
              ]),
            d.storage.procs.length > 0 &&
              simpleGrid(
                {
                  process: {
                    title: 'Process',
                    columnType: 'text',
                    cellRenderer: processLinkCell((name) =>
                      sysmonState.openSnapSubtab(snap, 'process', name),
                    ),
                  },
                  read_mb: {title: 'Read', columnType: 'quantitative', cellRenderer: mbCell},
                  write_mb: {title: 'Write', columnType: 'quantitative', cellRenderer: mbCell},
                  io_wait_ms: {title: 'I/O Wait', columnType: 'quantitative', cellRenderer: msCell},
                },
                d.storage.procs.slice(0, 6).map((pr) => ({
                  process: pr.processName,
                  read_mb: pr.readMb,
                  write_mb: pr.writeMb,
                  io_wait_ms: pr.ioWaitMs,
                })),
              ),
          ],
        }),
    },
    {
      id: 'apps',
      score: appsScore,
      quiet: appsQuiet,
      render: () =>
        areaCard({
          id: 'apps',
          title: 'Apps & Lifecycle',
          icon: 'apps',
          color: '#16a34a',
          headline: topApp?.topApp
            ? topApp.topApp.split('.').slice(-2).join('.')
            : `${d.apps.buckets.reduce((a, b) => a + b.procCount, 0)} procs`,
          headlineSub:
            lmkKills > 0
              ? `${lmkKills} LMK kills`
              : `${d.apps.events.length} transitions`,
          barFrac: appsScore / maxScore,
          quiet: appsQuiet,
          quietSummary: topApp?.topApp
            ? `Top: ${topApp.topApp.split('.').slice(-2).join('.')} · ${d.apps.events.length} transitions · ${lmkKills} LMK kills`
            : `${d.apps.events.length} lifecycle events`,
          finding: topApp?.topApp
            ? `Front app \`${topApp.topApp}\` (${topApp.topDurS.toFixed(0)}s). ${topApp.fgCount + topApp.visCount + topApp.percCount} privileged, ${topApp.svcCount} service, ${topApp.cachedCount} cached, ${topApp.nativeCount} native processes.`
            : `${d.apps.events.length} recorded state transitions across ${d.apps.buckets.length} OOM buckets.`,
          evidence: appsEv,
          open: opened.has('apps'),
          onToggle: () => toggle('apps'),
          onOpenTab: () => sysmonState.openSnapSubtab(snap, 'apps'),
          openLabel: 'Open Apps & States',
          moreContent: () => [
            d.apps.buckets.length > 0 &&
              proportionBar(
                d.apps.buckets.map((b) => ({
                  label: oomBucketByIdx(b.bucketIdx)?.short ?? b.bucketName,
                  value: b.anonSwapMib,
                  display: `${b.anonSwapMib.toFixed(0)} MiB (${b.procCount})`,
                  color: oomBucketByIdx(b.bucketIdx)?.color,
                })),
              ),
            d.apps.events.length > 0 &&
              renderEventFeed(
                d.apps.events,
                (name) => sysmonState.openSnapSubtab(snap, 'process', name),
                undefined,
                6,
              ),
          ],
        }),
    },
    {
      id: 'thermal',
      score: thermalScore,
      quiet: thermalQuiet,
      render: () =>
        areaCard({
          id: 'thermal',
          title: 'Thermal & Power',
          icon: 'thermostat',
          color: '#ea580c',
          headline: p.hasThermal
            ? fmtTempC(p.peakTempC)
            : fmtPowerMw(p.avgPowerMw ?? totalMw),
          headlineSub:
            p.throttleEvents > 0
              ? `${p.throttleEvents} caps`
              : totalMw > 0
                ? fmtPowerMw(totalMw)
                : undefined,
          barFrac: thermalScore / maxScore,
          quiet: thermalQuiet,
          quietSummary: p.hasThermal
            ? `Peak ${fmtTempC(p.peakTempC)}${totalMw > 0 ? ` · ${fmtPowerMw(totalMw)}` : ''}, no throttling`
            : totalMw > 0
              ? `Avg ${fmtPowerMw(totalMw)}, no thermal counters`
              : 'Not present in this trace',
          finding: `Peak temperature reached ${fmtTempC(p.peakTempC)} with ${p.throttleEvents} cooling-device throttling updates${totalMw > 0 ? ` and ${fmtPowerMw(totalMw)} avg ODPM draw` : ''}.`,
          open: opened.has('thermal'),
          onToggle: () => toggle('thermal'),
          onOpenTab: () => sysmonState.openSnapSubtab(snap, 'thermal'),
          moreContent:
            subRails.length > 0 || d.thermal.zones.length > 0
              ? () => [
                  subRails.length > 0 &&
                    simpleGrid(
                      {
                        rail: {title: 'Top Power Rails', columnType: 'text', cellRenderer: codeCell},
                        avg_mw: {title: 'Avg Power', columnType: 'quantitative', cellRenderer: powerMwCell},
                      },
                      subRails.slice(0, 5).map((r) => ({
                        rail: r.rail.replace(/^power\.rails\./, ''),
                        avg_mw: r.avgMw,
                      })),
                    ),
                  d.thermal.zones.length > 0 &&
                    simpleGrid(
                      {
                        zone: {title: 'Hottest Thermal Zones', columnType: 'text', cellRenderer: codeCell},
                        avg_c: {title: 'Avg', columnType: 'quantitative', cellRenderer: tempCell},
                        peak_c: {title: 'Peak', columnType: 'quantitative', cellRenderer: tempCell},
                      },
                      d.thermal.zones.slice(0, 5).map((z) => ({
                        zone: z.zone,
                        avg_c: z.avgC,
                        peak_c: z.peakC,
                      })),
                    ),
                ]
              : undefined,
        }),
    },
  ];

  list.sort((a, b) => {
    if (a.quiet !== b.quiet) return a.quiet ? 1 : -1;
    return b.score - a.score;
  });
  return list;
}
