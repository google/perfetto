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
import {classNames} from '../../../ui/src/base/classnames';
import type {App} from '../../../ui/src/public/app';
import type {Engine} from '../../../ui/src/trace_processor/engine';
import {Anchor} from '../../../ui/src/widgets/anchor';
import {Button, ButtonVariant} from '../../../ui/src/widgets/button';
import {Callout} from '../../../ui/src/widgets/callout';
import {Intent} from '../../../ui/src/widgets/common';
import {Grid, GridCell, type GridColumn} from '../../../ui/src/widgets/grid';
import {Icon} from '../../../ui/src/widgets/icon';
import {MenuItem, PopupMenu} from '../../../ui/src/widgets/menu';
import {PopupPosition} from '../../../ui/src/widgets/popup';
import {RadioGroup} from '../../../ui/src/widgets/radio_group';
import {Spinner} from '../../../ui/src/widgets/spinner';
import {Tabs, type TabsTab} from '../../../ui/src/widgets/tabs';
import {ConnectionPage} from '../../../ui/src/plugins/dev.perfetto.Memscope/views/connection';
import {
  disclosureMore,
  limitCallout,
  pageShell,
  questionBlock,
  stat,
} from '../../common/ui/kit/blocks';
import {
  proportionBar,
  type LiveStripContext,
} from '../../common/ui/kit/charts';
import {
  fmtBytes,
  fmtCount,
  fmtDeltaBytes,
  fmtPct,
  fmtPowerMw,
  fmtRateMb,
  fmtTempC,
} from '../../common/ui/kit/format';
import {
  colorChip,
  oomBucketFor,
  scorecardTile,
  sparklineSvg,
} from '../../common/ui/kit/sparkline';
import {
  chartCard,
  cpuBreakdownChart,
  cpuCoreHeatmapChart,
  cpuCoresBarChart,
  cpuFreqChart,
  cpuPressureChart,
  memoryCompositionChart,
  memoryPagingChart,
  memoryPsiChart,
  memorySummaryLineChart,
  powerRailsChart,
  storageDirtyChart,
  storagePsiChart,
  storageThroughputChart,
  systemPsiChart,
  thermalZonesChart,
} from './charts';
import {
  sysmonState,
  type LiveProcessTab,
  type LiveSnapshotViewData,
} from './state';
import {
  codeCell,
  countCell,
  freqKhzCell,
  pctCell,
  powerMwCell,
  simpleGrid,
  tempCell,
} from './tables';
import {TaskManagerTable} from './task_manager';
import {AppsTabView, renderEventFeed} from './views/apps_view';
import {LiveProcessTabView} from './views/process_view';
import {CURATED_WORKLOADS} from './workloads';

type MemRankMode = 'anon_swap' | 'dmabuf' | 'growth';

export function LiveView(): m.Component<{readonly app: App}> {
  let memRankMode: MemRankMode = 'anon_swap';
  let showRecentEvents = false;

  return {
    view({attrs: {app}}) {
      const live = sysmonState.liveSession;
      if (!live) {
        return m('.pf-sysmon-connect-wrap', [
          m(ConnectionPage, {
            app,
            title: 'Connect a device',
            description:
              'Monitor live CPU, memory, storage, apps, and thermal telemetry, or take snapshots.',
            onConnected: (res) => sysmonState.connectDevice(res),
          }),
        ]);
      }

      const data = live.data;
      const openProc = (name: string) => sysmonState.openLiveProcessTab(name);
      const liveCtx: LiveStripContext | undefined =
        data?.pulse !== undefined
          ? {
              baseSec: Number(data.pulse.startTs / 1_000_000_000n),
              latestSec: Number(data.pulse.endTs) / 1e9,
              isLive: !live.isPaused,
              windowSec: 60,
              lagSec: 2.0,
            }
          : undefined;

      const processTabs: TabsTab[] = sysmonState.liveProcessTabs.map(
        (pt: LiveProcessTab) => ({
          key: pt.key,
          leftIcon: m(Icon, {icon: 'apps', style: {color: '#2563eb'}}),
          closeButton: true,
          lazy: true,
          title: m(
            'span.pf-prod-scope-tab',
            {
              className: classNames(!pt.kept && 'pf-prod-scope-tab--preview'),
              title: pt.kept
                ? pt.processName
                : `${pt.processName} (preview — double-click to keep)`,
              ondblclick: () => {
                pt.kept = true;
              },
            },
            m('span.pf-prod-scope-tab__text', pt.processName),
          ),
          content: m(LiveProcessTabView, {
            engine: live.engine,
            tickGen: sysmonState.liveTickGen,
            win: data?.win,
            processName: pt.processName,
            row: data?.topProcs.find((p) => p.processName === pt.processName),
            events: data?.apps.events ?? [],
            devicePulse: data?.pulse,
            liveCtx,
          }),
        }),
      );

      const tabs: TabsTab[] = [
        {
          key: 'overview',
          title: m('span.pf-prod-pinned-tab', 'Overview'),
          leftIcon: 'dashboard',
          content: renderLiveOverview(
            data,
            liveCtx,
            openProc,
            showRecentEvents,
            () => {
              showRecentEvents = !showRecentEvents;
            },
          ),
        },
        {
          key: 'apps',
          title: 'Apps',
          leftIcon: 'apps',
          content: renderLiveApps(live.engine, data, liveCtx, openProc),
        },
        {
          key: 'cpu',
          title: 'CPU',
          leftIcon: 'speed',
          content: renderLiveCpu(data, liveCtx),
        },
        {
          key: 'memory',
          title: 'Memory',
          leftIcon: 'memory',
          content: renderLiveMemory(
            data,
            liveCtx,
            memRankMode,
            (mode) => {
              memRankMode = mode;
            },
            openProc,
          ),
        },
        {
          key: 'storage',
          title: 'Storage',
          leftIcon: 'hard_drive',
          content: renderLiveStorage(data, liveCtx),
        },
        {
          key: 'thermal',
          title: 'Thermal & Power',
          leftIcon: 'thermostat',
          content: renderLiveThermal(data, liveCtx),
        },
        ...processTabs,
      ];

      return pageShell(
        {
          title: [
            m('span.pf-sysmon-live-dot', {
              className: live.isPaused ? 'pf-sysmon-live-dot--paused' : undefined,
            }),
            live.deviceName,
          ],
          facts: [{icon: 'sensors', text: live.status}],
          actions: [
            m(Button, {
              label: sysmonState.capturingSnapshot
                ? 'Snapshotting…'
                : 'Take Snapshot',
              icon: 'photo_camera',
              intent: Intent.Primary,
              variant: ButtonVariant.Filled,
              disabled: sysmonState.capturingSnapshot || live.releasedForHandoff,
              onclick: () => void sysmonState.takeInstantSnapshot(),
            }),
            live.hasShell &&
              m(
                PopupMenu,
                {
                  position: PopupPosition.BottomEnd,
                  trigger: m(Button, {
                    label: sysmonState.runningWorkload
                      ? `Running ${sysmonState.runningWorkload}…`
                      : 'Run workload',
                    icon: 'bolt',
                    rightIcon: 'arrow_drop_down',
                    variant: ButtonVariant.Outlined,
                    disabled: sysmonState.runningWorkload !== undefined,
                  }),
                },
                CURATED_WORKLOADS.map((w) =>
                  m(MenuItem, {
                    icon: w.icon,
                    label: `${w.label} — ${w.description}`,
                    onclick: () =>
                      void sysmonState.runWorkload({
                        workload: w.id,
                        durationS: 6,
                        snapshotAfter: true,
                      }),
                  }),
                ),
              ),
            m(Button, {
              label: live.isPaused ? 'Resume' : 'Pause',
              icon: live.isPaused ? 'play_arrow' : 'pause',
              variant: ButtonVariant.Minimal,
              onclick: () => (live.isPaused ? live.resume() : live.pause()),
            }),
            m(Button, {
              label: 'Disconnect',
              icon: 'link_off',
              variant: ButtonVariant.Minimal,
              onclick: () => void sysmonState.disconnectDevice(),
            }),
          ],
        },
        [
          live.releasedForHandoff &&
            m(
              Callout,
              {icon: 'info', intent: Intent.Primary},
              m('.pf-prod-boundary__row', [
                m(
                  'span',
                  'Device connection paused for Memscope. Reconnect here when done.',
                ),
                m(Button, {
                  label: 'Reconnect',
                  variant: ButtonVariant.Outlined,
                  compact: true,
                  onclick: () => void sysmonState.disconnectDevice(),
                }),
              ]),
            ),
          (live.error || sysmonState.lastError) &&
            m(
              Callout,
              {icon: 'error', intent: Intent.Danger},
              live.error ?? sysmonState.lastError,
            ),
          m(Tabs, {
            className: 'pf-prod-tabs',
            variant: 'underline',
            activeTabKey: sysmonState.liveSubtab,
            onTabChange: (k: string) => {
              sysmonState.liveSubtab = k;
            },
            onTabClose: (k: string) => sysmonState.closeLiveProcessTab(k),
            tabs,
          }),
        ],
      );
    },
  };
}

function buildHealthVerdict(data: LiveSnapshotViewData): {
  readonly status: 'ok' | 'warn' | 'bad';
  readonly text: string;
} {
  const p = data.pulse;
  if (!p) return {status: 'ok', text: 'Collecting telemetry…'};
  const issues: string[] = [];
  let bad = false;

  if (p.lmkdKills > 0) {
    issues.push(`${p.lmkdKills} LMK kill${p.lmkdKills > 1 ? 's' : ''}`);
    bad = true;
  }
  if ((p.memPsiFullPct ?? 0) >= 2 || (p.memPsiSomePct ?? 0) >= 15) {
    issues.push(`memory stall ${fmtPct(p.memPsiSomePct)}`);
    bad = true;
  } else if ((p.memPsiSomePct ?? 0) >= 5) {
    issues.push(`memory PSI ${fmtPct(p.memPsiSomePct)}`);
  }
  if ((p.cpuPsiSomePct ?? 0) >= 20) {
    issues.push(`CPU stall ${fmtPct(p.cpuPsiSomePct)}`);
    bad = true;
  } else if ((p.cpuPsiSomePct ?? 0) >= 10) {
    issues.push(`CPU PSI ${fmtPct(p.cpuPsiSomePct)}`);
  }
  if ((p.ioPsiSomePct ?? 0) >= 20) {
    issues.push(`storage stall ${fmtPct(p.ioPsiSomePct)}`);
    bad = true;
  } else if ((p.ioPsiSomePct ?? 0) >= 10) {
    issues.push(`IO PSI ${fmtPct(p.ioPsiSomePct)}`);
  }
  if ((p.peakTempC ?? 0) >= 75) {
    issues.push(`SoC ${fmtTempC(p.peakTempC)}`);
    bad = true;
  } else if (data.thermal.cooling.some((c) => c.lastState > 0)) {
    issues.push('thermal cooling active');
  }

  if (issues.length === 0) {
    return {
      status: 'ok',
      text: 'Healthy — low contention across CPU, memory, storage, and thermal.',
    };
  }
  return {
    status: bad ? 'bad' : 'warn',
    text: `Under pressure: ${issues.join(' · ')}.`,
  };
}

function metricChartTile(attrs: {
  readonly title: string;
  readonly icon: string;
  readonly color: string;
  readonly value: string;
  readonly sub?: string;
  readonly chart: m.Child;
  readonly onclick?: () => void;
}): m.Child {
  if (!attrs.chart) return undefined;
  return m(
    '.pf-sysmon-metric-tile',
    {
      className: classNames(
        attrs.onclick && 'pf-sysmon-metric-tile--clickable',
      ),
      style: {'--pf-sysmon-tile-color': attrs.color},
      onclick: attrs.onclick,
      title: attrs.onclick ? `Open ${attrs.title} tab` : undefined,
    },
    [
      m('.pf-sysmon-metric-tile__head', [
        m('span.pf-sysmon-metric-tile__title', [
          m(Icon, {icon: attrs.icon}),
          attrs.title,
        ]),
        m('span.pf-sysmon-metric-tile__right', [
          m('span.pf-sysmon-metric-tile__val', attrs.value),
          attrs.sub && m('span.pf-sysmon-metric-tile__sub', attrs.sub),
        ]),
      ]),
      attrs.chart,
    ],
  );
}

function renderLiveOverview(
  data: LiveSnapshotViewData | undefined,
  liveCtx: LiveStripContext | undefined,
  onOpenProcess: (name: string) => void,
  showRecentEvents: boolean,
  onToggleEvents: () => void,
): m.Children {
  if (!data || !data.pulse) {
    return m('.pf-sysmon-wait', [
      m(Spinner, {easing: true}),
      m('span', 'Collecting live samples…'),
    ]);
  }
  const p = data.pulse;
  const top = data.apps.topApp;
  const health = buildHealthVerdict(data);

  const memUsedPct =
    p.memTotalBytes && p.memAvailLastBytes !== null
      ? (100 * (p.memTotalBytes - p.memAvailLastBytes)) / p.memTotalBytes
      : null;

  const stoRate =
    (data.storage.summary?.readMbS ?? 0) + (data.storage.summary?.writeMbS ?? 0);
  const activeCooling = data.thermal.cooling.filter((c) => c.lastState > 0).length;
  const topRail = data.thermal.rails.find((r) => !r.rail.includes('VSYS_ALL'));
  const topRailName = topRail?.rail.replace(/^power\.rails\./, '');
  const privCount =
    (top?.fgCount ?? 0) + (top?.visCount ?? 0) + (top?.percCount ?? 0);
  const shortTopApp = top?.topApp
    ? top.topApp.split('.').slice(-2).join('.')
    : '—';

  const maxPsiPct = Math.max(
    p.cpuPsiSomePct ?? 0,
    p.memPsiSomePct ?? 0,
    p.ioPsiSomePct ?? 0,
  );
  const hasFreqSeries = data.cpu.freqSeries.length >= 2;
  const hasPowerSeries = data.thermal.powerSeries.length >= 2;
  const hasThermalSeries = data.thermal.zoneSeries.length >= 2;
  const peakFreqKhz = data.cpu.cores.reduce(
    (mx, c) => Math.max(mx, c.maxFreqKhz ?? 0),
    0,
  );

  // Build Windows Task Manager–style grid of live 60fps rolling strip charts.
  const chartTiles: m.Child[] = [
    metricChartTile({
      title: 'CPU',
      icon: 'speed',
      color: '#2563eb',
      value: fmtPct(p.cpuBusyPct),
      sub:
        p.cpuPsiSomePct !== null
          ? `PSI ${fmtPct(p.cpuPsiSomePct)} · ${p.cpuCount} cores`
          : `${p.cpuCount} cores`,
      chart: cpuBreakdownChart(data.cpu.series, 145, liveCtx),
      onclick: () => sysmonState.selectLive('cpu'),
    }),
    metricChartTile({
      title: 'Memory',
      icon: 'memory',
      color: '#7c3aed',
      value:
        p.memAvailLastBytes !== null
          ? `${fmtBytes(p.memAvailLastBytes)} free`
          : '—',
      sub:
        memUsedPct !== null ? `${fmtPct(memUsedPct, 0)} used` : undefined,
      chart: memorySummaryLineChart(data.mem.series, data.mem.lmkd, 145, liveCtx),
      onclick: () => sysmonState.selectLive('memory'),
    }),
    metricChartTile({
      title: 'Pressure Stalls (PSI)',
      icon: 'warning_amber',
      color: '#d97706',
      value: fmtPct(maxPsiPct),
      sub: `CPU ${fmtPct(p.cpuPsiSomePct, 0)} · Mem ${fmtPct(p.memPsiSomePct, 0)} · IO ${fmtPct(p.ioPsiSomePct, 0)}`,
      chart: systemPsiChart(
        data.cpu.series,
        data.mem.pressureSeries,
        data.storage.series,
        145,
        liveCtx,
      ),
      onclick: () => sysmonState.selectLive('cpu'),
    }),
    metricChartTile({
      title: 'Storage I/O',
      icon: 'hard_drive',
      color: '#0284c7',
      value: stoRate >= 0.1 ? fmtRateMb(stoRate) : 'Quiet',
      sub: data.storage.summary
        ? `R ${fmtRateMb(data.storage.summary.readMbS)} · W ${fmtRateMb(data.storage.summary.writeMbS)}`
        : undefined,
      chart: storageThroughputChart(data.storage.series, 145, liveCtx),
      onclick: () => sysmonState.selectLive('storage'),
    }),
    hasFreqSeries
      ? metricChartTile({
          title: 'CPU Frequency',
          icon: 'tune',
          color: '#0891b2',
          value:
            peakFreqKhz > 0
              ? `${(peakFreqKhz / 1_000_000).toFixed(2)} GHz`
              : '—',
          sub: 'Cluster clocks',
          chart: cpuFreqChart(data.cpu.freqSeries, 145, liveCtx),
          onclick: () => sysmonState.selectLive('cpu'),
        })
      : metricChartTile({
          title: 'Paging & Swap',
          icon: 'swap_horiz',
          color: '#0891b2',
          value: data.mem.reclaim
            ? `${fmtCount(data.mem.reclaim.pgmajfaultDelta)} faults`
            : '—',
          sub: data.mem.reclaim
            ? `Swap ${fmtCount(data.mem.reclaim.pswpinDelta)} in / ${fmtCount(data.mem.reclaim.pswpoutDelta)} out`
            : undefined,
          chart: memoryPagingChart(data.mem.pressureSeries, 145, liveCtx),
          onclick: () => sysmonState.selectLive('memory'),
        }),
    hasPowerSeries
      ? metricChartTile({
          title: 'Power Draw',
          icon: 'bolt',
          color: '#ea580c',
          value: fmtPowerMw(p.avgPowerMw),
          sub: topRailName ? `Top: ${topRailName}` : undefined,
          chart: powerRailsChart(data.thermal.powerSeries, 145, liveCtx),
          onclick: () => sysmonState.selectLive('thermal'),
        })
      : hasThermalSeries
        ? metricChartTile({
            title: 'Thermal',
            icon: 'thermostat',
            color: '#ea580c',
            value: fmtTempC(p.peakTempC),
            sub:
              activeCooling > 0
                ? `${activeCooling} cooling active`
                : 'No throttling',
            chart: thermalZonesChart(data.thermal.zoneSeries, 145, liveCtx),
            onclick: () => sysmonState.selectLive('thermal'),
          })
        : metricChartTile({
            title: 'Dirty & Writeback Cache',
            icon: 'layers',
            color: '#ea580c',
            value: data.storage.summary
              ? `${data.storage.summary.dirtyMib.toFixed(1)} MiB`
              : '—',
            sub: data.storage.summary
              ? `Writeback ${data.storage.summary.writebackMib.toFixed(1)} MiB`
              : undefined,
            chart: storageDirtyChart(data.storage.series, 145, liveCtx),
            onclick: () => sysmonState.selectLive('storage'),
          }),
  ].filter(Boolean);

  return m('.pf-prod-landing', [
    // 1. Subtle 1-line health verdict
    m('.pf-sysmon-health-bar', {className: `pf-sysmon-health-bar--${health.status}`}, [
      m('span.pf-prod-scorecard__dot', {
        className: `pf-prod-scorecard__dot--${health.status}`,
      }),
      m('span.pf-sysmon-health-bar__text', health.text),
      health.status !== 'ok' &&
        m(Button, {
          label: 'Take Snapshot',
          icon: 'photo_camera',
          compact: true,
          variant: ButtonVariant.Outlined,
          onclick: () => void sysmonState.takeInstantSnapshot('pressure'),
        }),
    ]),

    // 2. Compact KPI Scorecards (including Top App; full charts right below)
    m('.pf-prod-scorecards', [
      scorecardTile({
        label: 'Top App',
        icon: 'apps',
        value: shortTopApp,
        title: top?.topApp ?? undefined,
        sub: top?.topApp
          ? `${privCount} priv · ${top.cachedCount} cached · ${top.lmkdKills} LMK`
          : 'Waiting for OOM stats',
        status: (top?.lmkdKills ?? 0) > 0 ? 'bad' : 'ok',
        onclick: () => sysmonState.selectLive('apps'),
      }),
      scorecardTile({
        label: 'CPU',
        icon: 'speed',
        value: fmtPct(p.cpuBusyPct),
        sub:
          p.cpuPsiSomePct !== null
            ? `PSI ${fmtPct(p.cpuPsiSomePct)} · ${p.cpuCount} cores`
            : `${p.cpuCount} cores`,
        status:
          (p.cpuPsiSomePct ?? 0) >= 20 || (p.cpuBusyPct ?? 0) >= 85
            ? 'bad'
            : (p.cpuPsiSomePct ?? 0) >= 10 || (p.cpuBusyPct ?? 0) >= 60
              ? 'warn'
              : 'ok',
        onclick: () => sysmonState.selectLive('cpu'),
      }),
      scorecardTile({
        label: 'Memory',
        icon: 'memory',
        value:
          p.memAvailLastBytes !== null
            ? `${fmtBytes(p.memAvailLastBytes)} free`
            : '—',
        sub:
          memUsedPct !== null
            ? `${fmtPct(memUsedPct, 0)} used · PSI ${fmtPct(p.memPsiSomePct)}`
            : undefined,
        status:
          p.lmkdKills > 0 || (p.memPsiSomePct ?? 0) >= 15
            ? 'bad'
            : (p.memPsiSomePct ?? 0) >= 5 || (memUsedPct ?? 0) >= 85
              ? 'warn'
              : 'ok',
        onclick: () => sysmonState.selectLive('memory'),
      }),
      scorecardTile({
        label: 'Storage',
        icon: 'hard_drive',
        value: stoRate >= 0.1 ? fmtRateMb(stoRate) : 'Quiet',
        sub: data.storage.summary
          ? `IO PSI ${fmtPct(data.storage.summary.ioPsiSomePct)} · Dirty ${data.storage.summary.dirtyMib.toFixed(1)} MiB`
          : undefined,
        status:
          (p.ioPsiSomePct ?? 0) >= 20
            ? 'bad'
            : (p.ioPsiSomePct ?? 0) >= 10
              ? 'warn'
              : 'ok',
        onclick: () => sysmonState.selectLive('storage'),
      }),
      scorecardTile({
        label: 'Power & Battery',
        icon: 'bolt',
        value:
          p.avgPowerMw !== null
            ? fmtPowerMw(p.avgPowerMw)
            : p.battPct !== null
              ? `${p.battPct.toFixed(0)}%`
              : '—',
        sub: [
          p.battPct !== null ? `Batt ${p.battPct.toFixed(0)}%` : undefined,
          topRailName ? `Top: ${topRailName}` : undefined,
        ]
          .filter(Boolean)
          .join(' · ') || 'No power counters',
        status: (p.avgPowerMw ?? 0) >= 5000 ? 'warn' : 'ok',
        onclick: () => sysmonState.selectLive('thermal'),
      }),
      scorecardTile({
        label: 'Thermal',
        icon: 'thermostat',
        value: p.peakTempC !== null ? fmtTempC(p.peakTempC) : '—',
        sub:
          activeCooling > 0
            ? `${activeCooling} cooling device${activeCooling > 1 ? 's' : ''} active`
            : p.throttleEvents > 0
              ? `${p.throttleEvents} cooling updates`
              : 'No throttling',
        status:
          (p.peakTempC ?? 0) >= 75
            ? 'bad'
            : (p.peakTempC ?? 0) >= 55 || activeCooling > 0
              ? 'warn'
              : 'ok',
        onclick: () => sysmonState.selectLive('thermal'),
      }),
    ]),

    // 3. Windows Task Manager–style live metric line charts grid
    chartTiles.length > 0 && m('.pf-sysmon-metric-grid', chartTiles),

    // 4. Processes list (Task Manager)
    questionBlock(
      {
        question: 'Processes',
        description:
          'Showing highly privileged processes by default. Switch to Active or All, sort any column, or click a process to open its live tab.',
      },
      m(TaskManagerTable, {
        processes: data.topProcs,
        defaultScope: 'privileged',
        maxRows: 60,
        onOpenProcess,
      }),
    ),

    // 5. Recent app events collapsed at the bottom so Overview stays focused
    data.apps.events.length > 0 &&
      disclosureMore(
        {
          open: showRecentEvents,
          label: `Show recent app events (${data.apps.events.length})`,
          openLabel: 'Hide recent app events',
          onToggle: onToggleEvents,
        },
        () =>
          renderEventFeed(
            data.apps.events,
            onOpenProcess,
            (tab) => {
              if (tab === 'memory' || tab === 'apps') {
                sysmonState.selectLive(tab);
              }
            },
            10,
          ),
      ),
  ]);
}

function renderLiveApps(
  engine: Engine | undefined,
  data: LiveSnapshotViewData | undefined,
  liveCtx: LiveStripContext | undefined,
  onOpenProcess: (name: string) => void,
): m.Children {
  if (!data || !data.pulse) {
    return m('.pf-sysmon-wait', [
      m(Spinner, {easing: true}),
      m('span', 'Loading app states…'),
    ]);
  }
  return m(AppsTabView, {
    engine,
    tickKey: `live-${sysmonState.liveTickGen}`,
    win: data.win,
    durationS: data.pulse.durS,
    apps: data.apps,
    processes: data.topProcs,
    liveCtx,
    initialOomBucketIdx: sysmonState.liveAppsFilterBucketIdx,
    onOpenProcess,
    onNavigateTab: (tab) => {
      if (tab === 'memory' || tab === 'cpu' || tab === 'storage' || tab === 'thermal') {
        sysmonState.selectLive(tab);
      }
    },
  });
}

function renderLiveMemory(
  data: LiveSnapshotViewData | undefined,
  liveCtx: LiveStripContext | undefined,
  rankMode: MemRankMode,
  onRankMode: (m: MemRankMode) => void,
  onOpenProcess: (name: string) => void,
): m.Children {
  if (!data || !data.pulse) {
    return m('.pf-sysmon-wait', [
      m(Spinner, {easing: true}),
      m('span', 'Loading memory telemetry…'),
    ]);
  }
  const p = data.pulse;
  const r = data.mem.reclaim;
  const totalSwapMib = data.topProcs.reduce((s, pr) => s + pr.swapMib, 0);
  const zramMib = (p.memZramBytes ?? 0) / 1048576;
  const zramRatio =
    zramMib > 5 && totalSwapMib > zramMib
      ? `${(totalSwapMib / zramMib).toFixed(1)}×`
      : undefined;

  const palette: Record<string, string> = {
    'Anon (Processes)': '#7c3aed',
    'File Cache': '#0891b2',
    'Kernel & Slab': '#64748b',
    'DMA-BUF & GPU': '#ec4899',
    'ZRAM': '#f59e0b',
    'Free': '#16a34a',
    'Other / Reserved': '#cbd5e1',
  };

  const rankedProcs = [...data.topProcs]
    .sort((a, b) => {
      if (rankMode === 'dmabuf') return b.dmabufMib - a.dmabufMib;
      if (rankMode === 'growth') return b.memDeltaMib - a.memDeltaMib;
      return b.anonSwapMib - a.anonSwapMib;
    })
    .slice(0, 20);

  const memCols: GridColumn[] = [
    {key: 'proc', header: m(GridCell, 'Process'), maxInitialWidthPx: 280},
    {key: 'oom', header: m(GridCell, 'OOM')},
    {key: 'anon_swap', header: m(GridCell, 'Anon + Swap')},
    {key: 'trend', header: m(GridCell, 'RAM trend')},
    {key: 'delta', header: m(GridCell, 'Δ RAM')},
    {key: 'file', header: m(GridCell, 'File RSS')},
    {key: 'dmabuf', header: m(GridCell, 'DMA-BUF')},
  ];
  const memRows = rankedProcs.map((pr) => {
    const b = oomBucketFor(pr.oomScoreAdj);
    const deltaColor =
      pr.memDeltaMib >= 1
        ? 'var(--pf-color-danger)'
        : pr.memDeltaMib <= -1
          ? 'var(--pf-color-success)'
          : undefined;
    return [
      m(
        GridCell,
        m(Anchor, {onclick: () => onOpenProcess(pr.processName)}, m('code', pr.processName)),
      ),
      m(
        GridCell,
        b
          ? colorChip(`${b.short} (${pr.oomScoreAdj})`, b.color)
          : `${pr.oomScoreAdj ?? '—'}`,
      ),
      m(GridCell, {align: 'right'}, `${pr.anonSwapMib.toFixed(1)} MiB`),
      m(GridCell, sparklineSvg(pr.ramTrend ?? [], {width: 64, height: 16, minSpan: 0.5})),
      m(
        GridCell,
        {align: 'right', style: deltaColor ? {color: deltaColor} : undefined},
        Math.abs(pr.memDeltaMib) >= 0.05
          ? fmtDeltaBytes(pr.memDeltaMib * 1024 * 1024)
          : '0 B',
      ),
      m(GridCell, {align: 'right'}, `${pr.fileMib.toFixed(1)} MiB`),
      m(
        GridCell,
        {align: 'right'},
        pr.dmabufMib > 0.05 ? `${pr.dmabufMib.toFixed(1)} MiB` : '—',
      ),
    ];
  });

  return m('.pf-prod-landing', [
    questionBlock(
      {
        question: 'Physical RAM ledger & pressure',
        answer: `MemTotal ${fmtBytes(p.memTotalBytes)} · Available ${fmtBytes(p.memAvailLastBytes)} · Anon ${fmtBytes(p.memAnonBytes)} · File Cache ${fmtBytes(p.memCacheBytes)}${zramRatio ? ` · ZRAM ${zramRatio} ratio` : ''}.`,
        actions: [
          {
            label: 'Take Memory Snapshot',
            icon: 'photo_camera',
            onclick: () =>
              void sysmonState.takeInstantSnapshot('memory', 'memory'),
          },
        ],
      },
      [
        proportionBar(
          data.mem.ledger.map((row) => ({
            label: row.category,
            value: row.bytes,
            display: `${fmtBytes(row.bytes)} (${row.pctOfTotal.toFixed(0)}%)`,
            color: palette[row.category],
          })),
        ),
        r &&
          m('.pf-prod-stats', [
            stat('Memory PSI (some)', fmtPct(r.psiMemSomePct)),
            stat('Memory PSI (full)', fmtPct(r.psiMemFullPct)),
            stat('Major Faults', fmtCount(r.pgmajfaultDelta)),
            stat('Swap In / Out', `${fmtCount(r.pswpinDelta)} / ${fmtCount(r.pswpoutDelta)}`),
            stat(
              'ZRAM',
              fmtBytes(p.memZramBytes),
              zramRatio ? `~${zramRatio} compression` : undefined,
            ),
            stat(
              'LMKD Kills',
              fmtCount(r.lmkdKills),
              undefined,
              r.lmkdKills > 0 ? 'bad' : undefined,
            ),
          ]),
        chartCard(
          'Physical RAM composition over time',
          memoryCompositionChart(data.mem.series, data.mem.lmkd, 220, liveCtx),
        ),
        m('.pf-sysmon-chart-row', [
          chartCard(
            'Memory PSI stall (%) & LMK kills',
            memoryPsiChart(data.mem.pressureSeries, 185, data.mem.lmkd, liveCtx),
          ),
          chartCard(
            'Paging, swap & reclaim rates',
            memoryPagingChart(data.mem.pressureSeries, 185, liveCtx),
          ),
        ]),
      ],
    ),
    questionBlock(
      {
        question: 'Top memory consumers',
        description:
          'Switch between Anon + Swap, DMA-BUF holders, and 60s growth. Click any process to inspect its dedicated tab.',
      },
      [
        m('.pf-sysmon-oom-controls', [
          m(
            RadioGroup,
            {
              intent: Intent.Primary,
              selectedValue: rankMode,
              onValueChange: (v) => onRankMode(v as MemRankMode),
            },
            m(RadioGroup.Button, {value: 'anon_swap'}, 'Anon + Swap'),
            m(RadioGroup.Button, {value: 'dmabuf'}, 'DMA-BUF'),
            m(RadioGroup.Button, {value: 'growth'}, '60s Growth'),
          ),
        ]),
        m(Grid, {columns: memCols, rowData: memRows, fillHeight: false}),
      ],
    ),
    limitCallout({
      badge: 'Memscope',
      title: 'Need Java heap retention or native allocation stacks?',
      body: 'Open Memscope to capture on-demand Java heap graphs and heapprofd native profiles.',
      actionLabel: 'Open Memscope',
      onAction: () => void sysmonState.openInMemscope(),
    }),
  ]);
}

function renderLiveCpu(
  data: LiveSnapshotViewData | undefined,
  liveCtx: LiveStripContext | undefined,
): m.Children {
  if (!data || !data.pulse) {
    return m('.pf-sysmon-wait', [
      m(Spinner, {easing: true}),
      m('span', 'Loading CPU telemetry…'),
    ]);
  }
  const p = data.pulse;
  const freqOrPsi =
    data.cpu.freqSeries.length >= 2
      ? chartCard('Cluster frequency (MHz)', cpuFreqChart(data.cpu.freqSeries, 195, liveCtx))
      : chartCard('Busy vs CPU PSI (%)', cpuPressureChart(data.cpu.series, 195, liveCtx));

  return m('.pf-prod-landing', [
    questionBlock(
      {
        question: 'Live CPU utilization, core heatmap & frequency',
        answer: `CPU busy ${fmtPct(p.cpuBusyPct)} across ${p.cpuCount} cores · CPU PSI ${fmtPct(p.cpuPsiSomePct)} · I/O Wait ${fmtPct(p.cpuIowaitPct)}.`,
        actions: [
          {
            label: 'Take CPU Snapshot (adds scheduling waits)',
            icon: 'photo_camera',
            onclick: () => void sysmonState.takeInstantSnapshot('cpu', 'cpu'),
          },
        ],
      },
      [
        m('.pf-sysmon-chart-row', [
          chartCard(
            'CPU utilization breakdown (User / Kernel / I/O Wait / IRQ)',
            cpuBreakdownChart(data.cpu.series, 195, liveCtx),
          ),
          freqOrPsi,
        ]),
        chartCard(
          'Per-core utilization over time (Core × Time heatmap)',
          cpuCoreHeatmapChart(data.cpu.coreSeries),
        ),
        chartCard(
          'Per-core average breakdown',
          cpuCoresBarChart(data.cpu.cores, 180),
        ),
        simpleGrid(
          {
            cpu: {title: 'CPU', columnType: 'text', cellRenderer: codeCell},
            busy: {title: 'Busy %', columnType: 'quantitative', cellRenderer: pctCell},
            user: {title: 'User %', columnType: 'quantitative', cellRenderer: pctCell},
            sys: {title: 'Kernel %', columnType: 'quantitative', cellRenderer: pctCell},
            iow: {title: 'I/O Wait %', columnType: 'quantitative', cellRenderer: pctCell},
            avg_freq: {title: 'Avg Freq', columnType: 'quantitative', cellRenderer: freqKhzCell},
            max_freq: {title: 'Max Freq', columnType: 'quantitative', cellRenderer: freqKhzCell},
          },
          data.cpu.cores.map((c) => ({
            cpu: `CPU ${c.cpu}`,
            busy: c.busyPct,
            user: c.userPct,
            sys: c.sysPct,
            iow: c.iowaitPct,
            avg_freq: c.avgFreqKhz,
            max_freq: c.maxFreqKhz,
          })),
        ),
      ],
    ),
  ]);
}

function renderLiveStorage(
  data: LiveSnapshotViewData | undefined,
  liveCtx: LiveStripContext | undefined,
): m.Children {
  if (!data || !data.pulse) {
    return m('.pf-sysmon-wait', [
      m(Spinner, {easing: true}),
      m('span', 'Loading storage telemetry…'),
    ]);
  }
  const s = data.storage.summary;
  return m('.pf-prod-landing', [
    questionBlock(
      {
        question: 'Live storage throughput, dirty page cache & I/O pressure',
        answer: s
          ? `Read ${s.readMb.toFixed(1)} MB (${fmtRateMb(s.readMbS)}) · Write ${s.writeMb.toFixed(1)} MB (${fmtRateMb(s.writeMbS)}) · IO PSI ${fmtPct(s.ioPsiSomePct)} · Dirty ${s.dirtyMib.toFixed(1)} MiB.`
          : 'Waiting for diskstat / PSI samples.',
        actions: [
          {
            label: 'Take Storage Snapshot (adds per-process block I/O)',
            icon: 'photo_camera',
            onclick: () =>
              void sysmonState.takeInstantSnapshot('storage', 'storage'),
          },
        ],
      },
      [
        s &&
          m('.pf-prod-stats', [
            stat('Read', `${s.readMb.toFixed(1)} MB`, fmtRateMb(s.readMbS)),
            stat('Write', `${s.writeMb.toFixed(1)} MB`, fmtRateMb(s.writeMbS)),
            stat('IO PSI (some / full)', `${fmtPct(s.ioPsiSomePct)} / ${fmtPct(s.ioPsiFullPct)}`),
            stat('Dirty / Writeback', `${s.dirtyMib.toFixed(1)} / ${s.writebackMib.toFixed(1)} MiB`),
          ]),
        m('.pf-sysmon-chart-row', [
          chartCard(
            'Read & Write throughput',
            storageThroughputChart(data.storage.series, 195, liveCtx),
          ),
          chartCard(
            'Dirty & Writeback page cache',
            storageDirtyChart(data.storage.series, 195, liveCtx),
          ),
        ]),
        chartCard(
          'Storage I/O PSI stall (%)',
          storagePsiChart(data.storage.series, 180, liveCtx),
        ),
      ],
    ),
  ]);
}

function renderLiveThermal(
  data: LiveSnapshotViewData | undefined,
  liveCtx: LiveStripContext | undefined,
): m.Children {
  if (!data || !data.pulse) {
    return m('.pf-sysmon-wait', [
      m(Spinner, {easing: true}),
      m('span', 'Loading thermal & power telemetry…'),
    ]);
  }
  const p = data.pulse;
  const activeCooling = data.thermal.cooling.filter((c) => c.lastState > 0);
  const topRail = data.thermal.rails.find((r) => !r.rail.includes('VSYS_ALL'));

  return m('.pf-prod-landing', [
    questionBlock(
      {
        question: 'Power draw (ODPM) & battery',
        answer:
          p.avgPowerMw !== null
            ? `Average whole-device power ${fmtPowerMw(p.avgPowerMw)}${topRail ? ` · top rail ${topRail.rail.replace(/^power\.rails\./, '')} (${fmtPowerMw(topRail.avgMw)})` : ''}${p.battPct !== null ? ` · Battery ${p.battPct.toFixed(0)}%` : ''}.`
            : p.battPct !== null
              ? `Battery ${p.battPct.toFixed(0)}% (${p.battCurrentMa !== null ? `${p.battCurrentMa} mA` : '—'}, ${p.battVoltageV !== null ? `${p.battVoltageV} V` : '—'}). No ODPM power rails reported.`
              : 'No ODPM power rails or battery counters reported.',
      },
      [
        m('.pf-prod-stats', [
          stat('Avg Power', fmtPowerMw(p.avgPowerMw), 'Whole device'),
          stat(
            'Battery',
            p.battPct !== null ? `${p.battPct.toFixed(0)}%` : '—',
            p.battVoltageV !== null
              ? `${p.battVoltageV.toFixed(2)} V${p.battCurrentMa !== null ? ` · ${p.battCurrentMa.toFixed(0)} mA` : ''}`
              : undefined,
          ),
          stat(
            'Peak Temp',
            fmtTempC(p.peakTempC),
            activeCooling.length > 0
              ? `${activeCooling.length} cooling active`
              : 'No active cooling',
            (p.peakTempC ?? 0) >= 75
              ? 'bad'
              : activeCooling.length > 0
                ? 'warn'
                : undefined,
          ),
          stat('Cooling updates', fmtCount(p.throttleEvents)),
        ]),
        chartCard(
          'ODPM power rails over time (stacked sub-rails)',
          powerRailsChart(data.thermal.powerSeries, 215, liveCtx),
        ),
        data.thermal.rails.length > 0 &&
          simpleGrid(
            {
              rail: {title: 'Power Rail', columnType: 'text', cellRenderer: codeCell},
              avg_mw: {title: 'Avg Power', columnType: 'quantitative', cellRenderer: powerMwCell},
              energy: {title: 'Energy (mJ)', columnType: 'quantitative'},
            },
            data.thermal.rails.map((r) => ({
              rail: r.rail,
              avg_mw: r.avgMw,
              energy: Number(r.energyMj.toFixed(1)),
            })),
          ),
      ],
    ),
    data.thermal.cooling.length > 0 &&
      questionBlock(
        {
          question: 'Thermal cooling devices',
          answer: `${activeCooling.length} of ${data.thermal.cooling.length} cooling devices currently throttling.`,
        },
        simpleGrid(
          {
            device: {title: 'Cooling Device', columnType: 'text', cellRenderer: codeCell},
            last: {title: 'Current State', columnType: 'quantitative', cellRenderer: countCell},
            max: {title: 'Peak State', columnType: 'quantitative', cellRenderer: countCell},
            updates: {title: 'Updates', columnType: 'quantitative', cellRenderer: countCell},
          },
          data.thermal.cooling.map((c) => ({
            device: c.device,
            last: c.lastState,
            max: c.maxState,
            updates: c.updates,
          })),
        ),
      ),
    questionBlock(
      {
        question: 'Thermal zones',
        answer:
          data.thermal.zones.length > 0
            ? `Peak ${fmtTempC(p.peakTempC)} across ${data.thermal.zones.length} sensors.`
            : 'No thermal zone counters reported.',
      },
      [
        chartCard(
          'Top thermal zones over time (°C)',
          thermalZonesChart(data.thermal.zoneSeries, 200, liveCtx),
        ),
        data.thermal.zones.length > 0 &&
          simpleGrid(
            {
              zone: {title: 'Thermal Zone', columnType: 'text', cellRenderer: codeCell},
              min: {title: 'Min', columnType: 'quantitative', cellRenderer: tempCell},
              avg: {title: 'Avg', columnType: 'quantitative', cellRenderer: tempCell},
              peak: {title: 'Peak', columnType: 'quantitative', cellRenderer: tempCell},
            },
            data.thermal.zones.map((z) => ({
              zone: z.zone,
              min: z.minC,
              avg: z.avgC,
              peak: z.peakC,
            })),
          ),
      ],
    ),
  ]);
}
