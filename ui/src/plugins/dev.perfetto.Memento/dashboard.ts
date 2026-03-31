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
import {App} from '../../public/app';
import {Button, ButtonVariant} from '../../widgets/button';
import {Intent} from '../../widgets/common';
import {PopupMenu} from '../../widgets/menu';
import {Select} from '../../widgets/select';
import {SegmentedButtons} from '../../widgets/segmented_buttons';
import {MementoSession} from './memento_session';
import {
  buildCategoryTimeSeries,
  buildOomScoreTimeSeries,
  buildCategoryDrilldown,
  buildOomDrilldown,
  buildLatestProcessMemory,
  buildPageCacheTimeSeries,
  buildFileCacheBreakdownTimeSeries,
  buildFileCacheActivityTimeSeries,
  buildPsiTimeSeries,
  buildPageFaultTimeSeries,
  buildSwapTimeSeries,
  buildVmstatTimeSeries,
  buildProcessMemoryBreakdown,
  computeT0,
} from './chart_builders';
import {renderProcessProfilePage} from './process_profile_page';
import {
  renderProcessesTab,
  type ProcessGrouping,
  type ProcessMetric,
  PROCESS_METRIC_OPTIONS,
} from './tab_processes';
import {type CategoryId, CATEGORIES} from './process_categories';
import {renderSystemTab} from './tab_system';
import {renderPageCacheTab} from './tab_page_cache';
import {renderPressureSwapTab} from './tab_pressure_swap';
import {OOM_SCORE_BUCKETS} from './tab_processes';
import {Chip} from '../../widgets/chip';
import {ProcessProfile} from './process_profile';

type Tab = 'processes' | 'system' | 'file_cache' | 'pressure_swap';

const INTERVAL_OPTIONS = [
  {label: '1s', ms: 1_000},
  {label: '3s', ms: 3_000},
  {label: '5s', ms: 5_000},
  {label: '10s', ms: 10_000},
  {label: '30s', ms: 30_000},
];

interface DashboardAttrs {
  readonly app: App;
  readonly session: MementoSession;
  readonly onStopped: () => void;
}

export class Dashboard implements m.ClassComponent<DashboardAttrs> {
  private activeTab: Tab = 'processes';
  private processGrouping: ProcessGrouping = 'category';
  private processMetric: ProcessMetric = 'rss';
  private selectedCategory?: CategoryId;
  private selectedOomBucket?: number;
  private activeProfile?: ProcessProfile;
  private profileStartTime?: number;
  private profileBaseline?: {anonSwap: number; file: number; dmabuf: number};

  view({attrs}: m.CVnode<DashboardAttrs>) {
    return m(
      '.pf-memento-page__container',
      m(
        '.pf-memento-page',

        // Title bar with status and actions (always shown).
        this.renderTitleBar(attrs, {
          showStopAndOpen: !this.activeProfile,
        }),

        // Profile page or dashboard tabs.
        this.activeProfile
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
      m('.pf-memento-title-bar__left', m('h1', 'Memento')),
      m(
        '.pf-memento-title-bar__actions',
        m('.pf-memento-status-bar__dot', {
          class: session.isPaused ? 'pf-memento-status-bar__dot--paused' : '',
        }),
        m(
          '.pf-memento-title-bar__device',
          session.deviceName,
          session.data?.isUserDebug &&
            m(Chip, {label: 'userdebug', intent: Intent.Warning}),
        ),
        m(
          PopupMenu,
          {
            trigger: m(Button, {
              label: `Snapshot #${session.snapshotCount}`,
              icon: 'info',
              minimal: true,
            }),
          },
          m(
            '.pf-memento-snapshot-info',
            m(
              '.pf-memento-snapshot-info__row',
              m('span', 'Interval'),
              m(
                Select,
                {
                  onchange: (e: Event) => {
                    const ms = Number((e.target as HTMLSelectElement).value);
                    session.setSnapshotInterval(ms);
                  },
                },
                INTERVAL_OPTIONS.map((opt) =>
                  m(
                    'option',
                    {
                      value: opt.ms,
                      selected: session.snapshotIntervalMs === opt.ms,
                    },
                    opt.label,
                  ),
                ),
              ),
            ),
            session.lastSnapshotMs > 0 && [
              m(
                '.pf-memento-snapshot-info__row',
                m('span', 'Total'),
                m('span', `${session.lastSnapshotMs}ms`),
              ),
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
            ],
            session.snapshotOverrun &&
              m(
                '.pf-memento-snapshot-info__warning',
                'Snapshot exceeded interval',
              ),
          ),
        ),
        m(Button, {
          label: session.isPaused ? 'Resume' : 'Pause',
          icon: session.isPaused ? 'play_arrow' : 'pause',
          onclick: () => {
            session.togglePause();
            m.redraw();
          },
        }),
        m(Button, {
          label: 'Disconnect',
          icon: 'usb_off',
          onclick: () => attrs.onStopped(),
        }),
        opts.showStopAndOpen &&
          m(Button, {
            label: 'Stop & Open Trace',
            icon: 'open_in_new',
            variant: ButtonVariant.Filled,
            intent: Intent.Primary,
            disabled: session.lastTraceBuffer === undefined,
            onclick: () => this.stopAndOpenTrace(attrs),
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
      this.activeTab === 'processes' && this.renderProcessesTab(attrs),
      this.activeTab === 'system' && renderSystemTab(session),
      this.activeTab === 'file_cache' && this.renderPageCacheTab(session),
      this.activeTab === 'pressure_swap' && this.renderPressureSwapTab(session),
      !session.data && m('.pf-memento-placeholder', 'Waiting for data\u2026'),
    ];
  }

  private renderProcessesTab(attrs: DashboardAttrs): m.Children {
    const {session} = attrs;
    const data = session.data;
    if (!data) return null;

    const t0 = computeT0(data);
    const counters = PROCESS_METRIC_OPTIONS.find(
      (o) => o.key === this.processMetric,
    )!.counters;

    const categoryChartData =
      this.processGrouping === 'category'
        ? buildCategoryTimeSeries(data, t0, counters)
        : buildOomScoreTimeSeries(data, t0, counters);

    const drilldownChartData =
      this.selectedCategory !== undefined
        ? buildCategoryDrilldown(data, this.selectedCategory, t0, counters)
        : this.selectedOomBucket !== undefined
          ? buildOomDrilldown(data, this.selectedOomBucket, t0, counters)
          : undefined;

    const latestProcesses = buildLatestProcessMemory(data);

    return renderProcessesTab(
      {
        processGrouping: this.processGrouping,
        processMetric: this.processMetric,
        selectedCategory: this.selectedCategory,
        selectedOomBucket: this.selectedOomBucket,
        categoryChartData,
        drilldownChartData,
        latestProcesses,
        xAxisMin: undefined,
        xAxisMax: undefined,
        heapProfilePid: this.activeProfile?.pid,
        heapProfileProcessName: this.activeProfile?.processName,
        heapProfileStopping: this.activeProfile?.state === 'stopping',
        isUserDebug: data.isUserDebug,
      },
      {
        onGroupingChange: (grouping) => {
          this.processGrouping = grouping;
          this.selectedCategory = undefined;
          this.selectedOomBucket = undefined;
        },
        onMetricChange: (metric) => {
          this.processMetric = metric;
          this.selectedCategory = undefined;
          this.selectedOomBucket = undefined;
        },
        onClearDrilldown: () => {
          this.selectedCategory = undefined;
          this.selectedOomBucket = undefined;
        },
        onSeriesClick: (seriesName) => {
          if (this.processGrouping === 'category') {
            const catIds = Object.keys(CATEGORIES) as CategoryId[];
            const id = catIds.find((k) => CATEGORIES[k].name === seriesName);
            if (id) this.selectedCategory = id;
          } else {
            const idx = OOM_SCORE_BUCKETS.findIndex(
              (b) => b.name === seriesName,
            );
            if (idx !== -1) this.selectedOomBucket = idx;
          }
        },
        onStartProfile: (pid, processName) => {
          this.startProfile(attrs, pid, processName);
        },
        onStopProfile: () => {
          this.stopProfile(attrs);
        },
        onCancelProfile: () => {
          this.cancelProfile();
        },
      },
    );
  }

  private renderPageCacheTab(session: MementoSession): m.Children {
    const data = session.data;
    if (!data) return null;

    const t0 = computeT0(data);
    return renderPageCacheTab({
      pageCacheChartData: buildPageCacheTimeSeries(data, t0),
      fileCacheBreakdownData: buildFileCacheBreakdownTimeSeries(data, t0),
      fileCacheActivityData: buildFileCacheActivityTimeSeries(data, t0),
      xAxisMin: undefined,
      xAxisMax: undefined,
    });
  }

  private renderPressureSwapTab(session: MementoSession): m.Children {
    const data = session.data;
    if (!data) return null;

    const t0 = computeT0(data);
    return renderPressureSwapTab({
      psiChartData: buildPsiTimeSeries(data, t0),
      pageFaultChartData: buildPageFaultTimeSeries(data, t0),
      swapChartData: buildSwapTimeSeries(data, t0),
      vmstatChartData: buildVmstatTimeSeries(data, t0),
      lmkEvents: data.lmkEvents,
      traceT0: t0,
      xAxisMin: undefined,
      xAxisMax: undefined,
    });
  }

  private renderProfilePage(attrs: DashboardAttrs): m.Children {
    const profile = this.activeProfile!;
    const {session} = attrs;
    const data = session.data;
    const t0 = data ? computeT0(data) : 0;
    const chartData = data
      ? buildProcessMemoryBreakdown(data, profile.pid, t0)
      : undefined;

    // Capture baseline from first chart data.
    if (this.profileBaseline === undefined && chartData !== undefined) {
      const first = (name: string): number => {
        const s = chartData.series.find((sr) => sr.name === name);
        return s !== undefined && s.points.length > 0 ? s.points[0].y : 0;
      };
      this.profileBaseline = {
        anonSwap: first('Anon + Swap'),
        file: first('File'),
        dmabuf: first('DMA-BUF'),
      };
    }

    return renderProcessProfilePage(
      {
        processName: profile.processName,
        pid: profile.pid,
        stopping: profile.state === 'stopping',
        duration: this.formatProfileDuration(),
        chartData,
        baseline: this.profileBaseline,
      },
      {
        onStop: () => this.stopProfile(attrs),
        onCancel: () => this.cancelProfile(),
      },
    );
  }

  private formatProfileDuration(): string {
    if (this.profileStartTime === undefined) return '';
    const elapsed = Math.floor((Date.now() - this.profileStartTime) / 1000);
    const mins = Math.floor(elapsed / 60);
    const secs = elapsed % 60;
    if (mins > 0) return `${mins}m ${secs}s`;
    return `${secs}s`;
  }

  private async startProfile(
    attrs: DashboardAttrs,
    pid: number,
    processName: string,
  ) {
    try {
      this.activeProfile = await attrs.session.startProcessProfile(
        pid,
        processName,
      );
      this.profileStartTime = Date.now();
      this.profileBaseline = undefined;
      m.redraw();
    } catch (e) {
      console.error('Profile failed:', e);
    }
  }

  private async stopProfile(attrs: DashboardAttrs) {
    const profile = this.activeProfile;
    if (!profile) return;
    await profile.stop();
    const traceData = profile.getTraceData();
    this.clearProfile();
    m.redraw();
    if (traceData) {
      const fileName = `heap-${profile.processName}-${profile.pid}.perfetto-trace`;
      const buffer = traceData.buffer as ArrayBuffer;
      attrs.app.openTraceFromBuffer({buffer, title: fileName, fileName});
    }
  }

  private async cancelProfile() {
    if (!this.activeProfile) return;
    await this.activeProfile.cancel();
    this.clearProfile();
    m.redraw();
  }

  private clearProfile() {
    this.activeProfile = undefined;
    this.profileStartTime = undefined;
    this.profileBaseline = undefined;
  }

  private async stopAndOpenTrace(attrs: DashboardAttrs) {
    const buffer = attrs.session.lastTraceBuffer;
    if (buffer === undefined) return;
    const fileName = `live-memory-${Date.now()}.perfetto-trace`;
    await attrs.session.dispose();
    attrs.app.openTraceFromBuffer({buffer, title: fileName, fileName});
  }
}
