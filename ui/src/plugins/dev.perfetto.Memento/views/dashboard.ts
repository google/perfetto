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
import {App} from '../../../public/app';
import {Button, ButtonVariant} from '../../../widgets/button';
import {Intent} from '../../../widgets/common';
import {SegmentedButtons} from '../../../widgets/segmented_buttons';
import {
  type LineChartData,
  type LineChartSeries,
} from '../../../components/widgets/charts/line_chart';
import {LiveSession, type SnapshotData} from '../sessions/live_session';
import {ProfilePage} from './profile_page';
import {ProcessesTab} from './tabs/processes';
import {renderSystemTab} from './tabs/system';
import {renderPageCacheTab} from './tabs/page_cache';
import {renderPressureSwapTab} from './tabs/pressure_swap';
import {Chip} from '../../../widgets/chip';
import {Tooltip} from '../../../widgets/tooltip';
import {PopupPosition} from '../../../widgets/popup';

type Tab = 'processes' | 'system' | 'file_cache' | 'pressure_swap';

function buildProcessMemoryBreakdown(
  data: SnapshotData,
  pid: number,
  t0: number,
): LineChartData | undefined {
  const pidCounters = data.processCountersByPid.get(pid);
  if (pidCounters === undefined) return undefined;
  const SERIES_NAMES = ['Anon + Swap', 'File', 'DMA-BUF'] as const;
  const counterMapping: Record<string, string> = {
    'mem.rss.anon': 'Anon + Swap',
    'mem.swap': 'Anon + Swap',
    'mem.rss.file': 'File',
    'mem.dmabuf_rss': 'DMA-BUF',
  };
  const tsSet = new Set<number>();
  const bySeriesTs = new Map<number, Map<string, number>>();
  for (const [counterName, samples] of pidCounters) {
    const seriesName = counterMapping[counterName];
    if (seriesName === undefined) continue;
    for (const {ts, value} of samples) {
      tsSet.add(ts);
      let seriesMap = bySeriesTs.get(ts);
      if (seriesMap === undefined) {
        seriesMap = new Map();
        bySeriesTs.set(ts, seriesMap);
      }
      seriesMap.set(
        seriesName,
        (seriesMap.get(seriesName) ?? 0) + Math.round(value / 1024),
      );
    }
  }
  const timestamps = [...tsSet].sort((a, b) => a - b);
  if (timestamps.length < 2) return undefined;
  const colors: Record<string, string> = {
    'Anon + Swap': '#ff9800',
    'File': '#4caf50',
    'DMA-BUF': '#2196f3',
  };
  const series: LineChartSeries[] = [];
  for (const name of SERIES_NAMES) {
    const points = timestamps.map((ts) => ({
      x: (ts - t0) / 1e9,
      y: bySeriesTs.get(ts)?.get(name) ?? 0,
    }));
    if (points.some((p) => p.y > 0)) {
      series.push({name, points, color: colors[name]});
    }
  }
  if (series.length === 0) return undefined;
  return {series};
}

interface DashboardAttrs {
  readonly app: App;
  readonly session: LiveSession;
  readonly onStopped: () => void;
}

export class Dashboard implements m.ClassComponent<DashboardAttrs> {
  private activeTab: Tab = 'processes';
  private profileBaseline?: {anonSwap: number; file: number; dmabuf: number};

  view({attrs}: m.CVnode<DashboardAttrs>) {
    const {session} = attrs;
    return m(
      '.pf-memento-page__container',
      m(
        '.pf-memento-page',

        // Title bar with status and actions (always shown).
        this.renderTitleBar(attrs, {showStopAndOpen: !session.isProfiling}),

        // Profile page or dashboard content.
        session.isProfiling
          ? this.renderProfilePage(attrs)
          : this.renderDashboard(attrs),
      ),
    );
  }

  private renderTitleBar(
    attrs: DashboardAttrs,
    opts: {showStopAndOpen: boolean} = {showStopAndOpen: true},
  ): m.Children {
    const {session} = attrs;
    return m(
      '.pf-memento-title-bar',
      // Left: title + device identity.
      m(
        '.pf-memento-title-bar__left',
        m('h1', 'Memento'),
        m('.pf-memento-title-bar__sep'),
        m(
          '.pf-memento-title-bar__device',
          m('.pf-memento-status-bar__dot', {
            class: session.isPaused ? 'pf-memento-status-bar__dot--paused' : '',
          }),
          m('span', session.deviceName),
          session.data?.isUserDebug &&
            m(Chip, {label: 'userdebug', intent: Intent.Warning}),
        ),
      ),

      // Right: info controls + action buttons.
      m(
        '.pf-memento-title-bar__actions',
        // Snapshot chip with timing tooltip.
        m(
          Tooltip,
          {
            trigger: m(
              '.pf-memento--muted',
              `Snapshot #${session.snapshotCount}`,
            ),
            position: PopupPosition.Bottom,
          },
          m(
            '.pf-memento-snapshot-info',
            session.lastSnapshotMs > 0
              ? [
                  m(
                    '.pf-memento-snapshot-info__row',
                    m('span', 'Size'),
                    m('span', `${session.lastSnapshotSizeKb.toFixed(0)}kB`),
                  ),
                  session.lastBufferUsagePct !== undefined &&
                    m(
                      '.pf-memento-snapshot-info__row',
                      m('span', 'Buffer usage'),
                      m('span', `${session.lastBufferUsagePct.toFixed(1)}%`),
                    ),
                  session.data !== undefined &&
                    m('.pf-memento-snapshot-info__heading', 'Counter range'),
                  session.data !== undefined &&
                    m(
                      '.pf-memento-snapshot-info__row',
                      m('span', 'First sample'),
                      m('span', `${session.data.xMin.toFixed(1)}s`),
                    ),
                  session.data !== undefined &&
                    m(
                      '.pf-memento-snapshot-info__row',
                      m('span', 'Last sample'),
                      m('span', `${session.data.xMax.toFixed(1)}s`),
                    ),
                  m('.pf-memento-snapshot-info__heading', 'Timings'),
                  m(
                    '.pf-memento-snapshot-info__row',
                    m('span', 'Clone'),
                    m('span', `${session.lastCloneMs}ms`),
                  ),
                  m(
                    '.pf-memento-snapshot-info__row',
                    m('span', 'Parse'),
                    m('span', `${session.lastParseMs}ms`),
                  ),
                  m(
                    '.pf-memento-snapshot-info__row',
                    m('span', 'Query'),
                    m('span', `${session.lastQueryMs}ms`),
                  ),
                  m(
                    '.pf-memento-snapshot-info__row',
                    m('span', 'Extract'),
                    m('span', `${session.lastExtractMs}ms`),
                  ),
                  m(
                    '.pf-memento-snapshot-info__row.pf-memento-snapshot-info__sum',
                    m('span', 'Total'),
                    m('span', `${session.lastSnapshotMs}ms`),
                  ),
                  session.snapshotOverrun &&
                    m(
                      '.pf-memento-snapshot-info__overrun',
                      m('span.material-icons', 'warning'),
                      'Exceeds interval — increase snapshot rate',
                    ),
                ]
              : m(
                  '.pf-memento-snapshot-info__empty',
                  'Waiting for snapshot\u2026',
                ),
          ),
        ),
        // Pause / Resume.
        m(Button, {
          label: session.isPaused ? 'Paused' : 'Live',
          icon: session.isPaused ? 'pause' : 'fiber_manual_record',
          intent: session.isPaused ? Intent.Warning : Intent.Success,
          variant: ButtonVariant.Filled,
          onclick: () => {
            session.togglePause();
            m.redraw();
          },
        }),
        // Stop & open trace (only when not profiling).
        opts.showStopAndOpen &&
          m(Button, {
            label: 'Stop & Open Trace',
            icon: 'open_in_new',
            variant: ButtonVariant.Filled,
            intent: Intent.Primary,
            disabled: session.lastTraceBuffer === undefined,
            onclick: () => this.stopAndOpenTrace(attrs),
          }),
        // Disconnect — secondary action, icon-only to reduce visual weight.
        m(Button, {
          icon: 'usb_off',
          label: 'Disconnect',
          minimal: true,
          variant: ButtonVariant.Filled,
          onclick: () => attrs.onStopped(),
        }),
      ),
    );
  }

  private renderDashboard(attrs: DashboardAttrs): m.Children {
    const {session} = attrs;
    return [
      // Tab strip.
      m(
        '.pf-memento-tabs',
        m(SegmentedButtons, {
          options: [
            {label: 'Processes', icon: 'apps'},
            {label: 'System', icon: 'monitoring'},
            {label: 'Page Cache', icon: 'file_copy'},
            {label: 'Pressure, Faults & Swap', icon: 'speed'},
          ],
          selectedOption: (
            ['processes', 'system', 'file_cache', 'pressure_swap'] as const
          ).indexOf(this.activeTab),
          onOptionSelected: (i: number) => {
            const tabs: Tab[] = [
              'processes',
              'system',
              'file_cache',
              'pressure_swap',
            ];
            this.activeTab = tabs[i];
          },
        }),
      ),

      // Tab content.
      this.activeTab === 'processes' && m(ProcessesTab, {session}),
      this.activeTab === 'system' && renderSystemTab(session),
      this.activeTab === 'file_cache' && renderPageCacheTab(session),
      this.activeTab === 'pressure_swap' && renderPressureSwapTab(session),
      !session.data && m('.pf-memento-placeholder', 'Waiting for data\u2026'),
    ];
  }

  private renderProfilePage(attrs: DashboardAttrs): m.Children {
    const {session} = attrs;
    const data = session.data;
    const pid = session.profilePid!;
    const processName = session.profileProcessName ?? 'unknown';
    const t0 = data?.ts0 ?? 0;
    const chartData = data
      ? buildProcessMemoryBreakdown(data, pid, t0)
      : undefined;
    const activeProfileSession = session.activeProfile;

    // Capture baseline from first chart data.
    if (this.profileBaseline === undefined && chartData !== undefined) {
      const first = (name: string): number => {
        const s = chartData.series.find(
          (sr: LineChartSeries) => sr.name === name,
        );
        return s !== undefined && s.points.length > 0 ? s.points[0].y : 0;
      };
      this.profileBaseline = {
        anonSwap: first('Anon + Swap'),
        file: first('File'),
        dmabuf: first('DMA-BUF'),
      };
    }

    return m(ProfilePage, {
      session: activeProfileSession!,
      processName,
      pid,
      stopping: session.profileState === 'stopping',
      duration: session.profileDuration,
      chartData,
      baseline: this.profileBaseline,
      xMin: data?.xMin ?? 0,
      xMax: data?.xMax ?? 0,
      startX: session.profileStartX,
      onStop: () => {
        session.stopAndOpenProfile().then(() => {
          this.profileBaseline = undefined;
          m.redraw();
        });
      },
      onCancel: () => {
        session.cancelProfile().then(() => {
          this.profileBaseline = undefined;
          m.redraw();
        });
      },
    });
  }

  private async stopAndOpenTrace(attrs: DashboardAttrs) {
    const buffer = attrs.session.lastTraceBuffer;
    if (buffer === undefined) return;
    const fileName = `live-memory-${Date.now()}.perfetto-trace`;
    await attrs.session.dispose();
    attrs.app.openTraceFromBuffer({buffer, title: fileName, fileName});
  }
}
