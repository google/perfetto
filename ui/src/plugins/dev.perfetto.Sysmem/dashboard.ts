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
import {GateDetector} from '../../base/mithril_utils';
import {Intent} from '../../widgets/common';
import {RadioGroup} from '../../widgets/radio_group';
import type {App} from '../../public/app';
import {
  type LiveSession,
  LiveProfileView,
  type LiveSessionAttrs,
  Page,
  PreviewBanner,
  renderSystemSummary,
  SessionPill,
  type SnapshotData,
} from '../dev.perfetto.Memscope/live';
import {renderPageCacheTab} from './tabs/page_cache';
import {renderPressureSwapTab} from './tabs/pressure_swap';
import {ProcessesTab} from './tabs/processes';

type Tab = 'processes' | 'system' | 'file_cache' | 'pressure_swap';

// Either a live session or a snapshot of a loaded trace (read-only).
export type DashboardAttrs =
  | {readonly live: LiveSessionAttrs}
  | {readonly app: App; readonly data: SnapshotData; readonly title: string};

export class Dashboard implements m.ClassComponent<DashboardAttrs> {
  private activeTab: Tab = 'processes';

  view({attrs}: m.CVnode<DashboardAttrs>) {
    if (!('live' in attrs)) {
      return m(
        Page,
        this.renderTitleBar(m('span', attrs.title), true),
        this.renderDashboard(attrs.app, attrs.data),
      );
    }
    const live = attrs.live;
    const {session} = live;
    return m(
      GateDetector,
      {
        onVisibilityChanged: (visible) =>
          visible ? session.resume() : session.pause(),
      },
      m(
        Page,
        this.renderTitleBar(
          m(SessionPill, live),
          session.profile === undefined,
        ),
        m(PreviewBanner, {app: live.app}),
        session.profile !== undefined
          ? m(LiveProfileView, live)
          : this.renderDashboard(live.app, session.data, session),
      ),
    );
  }

  private renderTitleBar(right: m.Children, showTabs: boolean): m.Children {
    return m(
      '.pf-memscope-title-bar',
      m(
        '.pf-memscope-title-bar__left',
        m('h1', 'Sysmem'),
        showTabs && this.renderTabStrip(),
      ),
      right,
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

  private renderDashboard(
    app: App,
    data: SnapshotData | undefined,
    session?: LiveSession,
  ): m.Children {
    const live = session !== undefined;
    return [
      this.activeTab === 'processes' && m(ProcessesTab, {app, data, session}),
      this.activeTab === 'system' && renderSystemSummary(data, live),
      this.activeTab === 'file_cache' && renderPageCacheTab(data, live),
      this.activeTab === 'pressure_swap' &&
        renderPressureSwapTab(app, data, live),
      !data && m('.pf-memscope-placeholder', 'Waiting for data\u2026'),
    ];
  }
}
