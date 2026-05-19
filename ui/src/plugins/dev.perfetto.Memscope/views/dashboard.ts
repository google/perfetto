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

import './dashboard.scss';
import m from 'mithril';
import type {App} from '../../../public/app';
import {Button, ButtonGroup, ButtonVariant} from '../../../widgets/button';
import {Intent} from '../../../widgets/common';
import {RadioGroup} from '../../../widgets/radio_group';
import {
  type LineChartData,
  type LineChartSeries,
} from '../../../components/widgets/charts/line_chart';
import {GateDetector} from '../../../base/mithril_utils';
import {
  LiveSession,
  type ProfileView,
  type SnapshotData,
} from '../sessions/live_session';
import {ProfilePage} from './profile_page';
import {ProcessesTab} from './tabs/processes';
import {renderSystemTab} from './tabs/system';
import {renderPageCacheTab} from './tabs/page_cache';
import {renderPressureSwapTab} from './tabs/pressure_swap';
import {Chip} from '../../../widgets/chip';
import {PopupPosition} from '../../../widgets/popup';
import {MenuDivider, MenuItem, PopupMenu} from '../../../widgets/menu';

type Tab = 'processes' | 'system' | 'file_cache' | 'pressure_swap';

function buildProcessMemoryBreakdown(
  data: SnapshotData,
  pid: number,
  t0: number,
): LineChartData | undefined {
  // SnapshotData keys process counters by upid; resolve from pid.
  let upid: number | undefined;
  for (const info of data.processInfo.values()) {
    if (info.pid === pid) {
      upid = info.upid;
      break;
    }
  }
  if (upid === undefined) return undefined;
  const pidCounters = data.processCountersByUpid.get(upid);
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
    'Anon + Swap': 'var(--pf-color-warning)',
    'File': 'var(--pf-color-success)',
    'DMA-BUF': 'var(--pf-color-primary)',
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
      GateDetector,
      {
        onVisibilityChanged: (visible) =>
          visible ? session.resume() : session.pause(),
      },
      m(
        '.pf-memscope-page__container',
        m(
          '.pf-memscope-page',

          // Title bar with status and actions (always shown).
          this.renderTitleBar(attrs),

          // Profile page or dashboard content.
          session.profile !== undefined
            ? this.renderProfilePage(attrs, session.profile)
            : this.renderDashboard(attrs),
        ),
      ),
    );
  }

  private renderTitleBar(attrs: DashboardAttrs): m.Children {
    return m(
      '.pf-memscope-title-bar',
      // Left: title + session pill (device + snapshot counter + pause/play).
      m(
        '.pf-memscope-title-bar__left',
        m('h1', 'Memscope'),
        attrs.session.profile === undefined && this.renderTabStrip(),
      ),
      this.renderSessionPill(attrs),
    );
  }

  private renderTabStrip(): m.Children {
    return m(
      RadioGroup,
      {
        intent: Intent.Primary,
        selectedValue: this.activeTab,
        onValueChange: (value) => {
          this.activeTab = value as Tab;
        },
      },
      m(RadioGroup.Button, {value: 'processes', icon: 'apps'}, 'Processes'),
      m(RadioGroup.Button, {value: 'system', icon: 'monitoring'}, 'System'),
      m(
        RadioGroup.Button,
        {value: 'file_cache', icon: 'file_copy'},
        'Page Cache',
      ),

      // Right: action buttons.
      m(
        RadioGroup.Button,
        {value: 'pressure_swap', icon: 'speed'},
        'Pressure, Faults & Swap',
      ),
    );
  }

  private renderSessionPill(attrs: DashboardAttrs): m.Children {
    const {session} = attrs;
    return m(
      '.pf-memscope-session-pill',
      // Device identity sits to the left of the button group.
      m(
        '.pf-memscope-session-pill__device',
        m('.pf-memscope-status-bar__dot', {
          class: session.isPaused ? 'pf-memscope-status-bar__dot--paused' : '',
        }),
        m('span', session.deviceName),
        session.data?.isUserDebug &&
          m(Chip, {label: 'userdebug', intent: Intent.Warning}),
      ),
      m(
        ButtonGroup,
        // Pause / Resume.
        m(Button, {
          variant: ButtonVariant.Filled,
          label: session.isPaused ? 'Resume' : 'Pause',
          icon: session.isPaused ? 'play_arrow' : 'pause',
          onclick: () => {
            session.togglePause();
            m.redraw();
          },
        }),
        // Overflow menu for session actions.
        m(
          PopupMenu,
          {
            trigger: m(Button, {
              variant: ButtonVariant.Filled,
              icon: 'more_vert',
            }),
            position: PopupPosition.BottomEnd,
          },

          m(MenuItem, {
            label: 'Stop & Open Trace',
            icon: 'open_in_new',
            disabled: session.lastTraceBuffer === undefined,
            onclick: () => this.stopAndOpenTrace(attrs),
          }),
          m(MenuItem, {
            label: 'Disconnect',
            icon: 'usb_off',
            onclick: () => attrs.onStopped(),
          }),
          m(MenuDivider),
          m(
            MenuItem,
            {
              label: 'Snapshot stats',
              icon: 'photo_camera',
            },
            this.renderSnapshotStats(session),
          ),
        ),
      ),
    );
  }

  private renderDashboard(attrs: DashboardAttrs): m.Children {
    const {session} = attrs;
    return [
      this.activeTab === 'processes' && m(ProcessesTab, {session}),
      this.activeTab === 'system' && renderSystemTab(session),
      this.activeTab === 'file_cache' && renderPageCacheTab(session),
      this.activeTab === 'pressure_swap' && renderPressureSwapTab(session),
      !session.data && m('.pf-memscope-placeholder', 'Waiting for data\u2026'),
    ];
  }

  private renderSnapshotStats(session: LiveSession): m.Children {
    if (session.lastSnapshotMs <= 0) {
      return m(
        '.pf-memscope-snapshot-info',
        m('.pf-memscope-snapshot-info__empty', 'Waiting for snapshot…'),
      );
    }
    return m(
      '.pf-memscope-snapshot-info',
      m('.pf-memscope-snapshot-info__heading', 'Snapshot'),
      m(
        '.pf-memscope-snapshot-info__row',
        m('span', 'Count'),
        m('span', `#${session.snapshotCount}`),
      ),
      m(
        '.pf-memscope-snapshot-info__row',
        m('span', 'Size'),
        m('span', `${session.lastSnapshotSizeKb.toFixed(0)}kB`),
      ),
      session.lastBufferUsagePct !== undefined &&
        m(
          '.pf-memscope-snapshot-info__row',
          m('span', 'Buffer usage'),
          m('span', `${session.lastBufferUsagePct.toFixed(1)}%`),
        ),
      session.data !== undefined && [
        m('.pf-memscope-snapshot-info__heading', 'Counter range'),
        m(
          '.pf-memscope-snapshot-info__row',
          m('span', 'First sample'),
          m('span', `${session.data.xMin.toFixed(1)}s`),
        ),
        m(
          '.pf-memscope-snapshot-info__row',
          m('span', 'Last sample'),
          m('span', `${session.data.xMax.toFixed(1)}s`),
        ),
      ],
      m('.pf-memscope-snapshot-info__heading', 'Timings'),
      m(
        '.pf-memscope-snapshot-info__row',
        m('span', 'Clone'),
        m('span', `${session.lastCloneMs}ms`),
      ),
      m(
        '.pf-memscope-snapshot-info__row',
        m('span', 'Parse'),
        m('span', `${session.lastParseMs}ms`),
      ),
      m(
        '.pf-memscope-snapshot-info__row',
        m('span', 'Query'),
        m('span', `${session.lastQueryMs}ms`),
      ),
      m(
        '.pf-memscope-snapshot-info__row',
        m('span', 'Extract'),
        m('span', `${session.lastExtractMs}ms`),
      ),
      m(
        '.pf-memscope-snapshot-info__row.pf-memscope-snapshot-info__sum',
        m('span', 'Total'),
        m('span', `${session.lastSnapshotMs}ms`),
      ),
      session.snapshotOverrun &&
        m(
          '.pf-memscope-snapshot-info__overrun',
          m('span.material-icons', 'warning'),
          'Exceeds interval — increase snapshot rate',
        ),
    );
  }

  private renderProfilePage(
    attrs: DashboardAttrs,
    profile: ProfileView,
  ): m.Children {
    const {session} = attrs;
    const data = session.data;
    const t0 = data?.ts0 ?? 0;
    const chartData = data
      ? buildProcessMemoryBreakdown(data, profile.pid, t0)
      : undefined;

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
      state: profile.state as 'recording' | 'stopping' | 'finished',
      bufferUsagePct: profile.bufferUsagePct,
      processName: profile.processName,
      pid: profile.pid,
      startMs: profile.startMs,
      chartData,
      baseline: this.profileBaseline,
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
