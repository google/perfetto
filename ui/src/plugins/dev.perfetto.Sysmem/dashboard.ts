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
import {
  LiveProfileView,
  type LiveSessionAttrs,
  Page,
  PreviewBanner,
  SessionPill,
} from '../dev.perfetto.Memscope/live';
import {renderPageCacheTab} from './tabs/page_cache';
import {renderPressureSwapTab} from './tabs/pressure_swap';
import {ProcessesTab} from './tabs/processes';
import {renderSystemTab} from './tabs/system';

type Tab = 'processes' | 'system' | 'file_cache' | 'pressure_swap';

type DashboardAttrs = LiveSessionAttrs;

export class Dashboard implements m.ClassComponent<DashboardAttrs> {
  private activeTab: Tab = 'processes';

  view({attrs}: m.CVnode<DashboardAttrs>) {
    const {session} = attrs;
    return m(
      GateDetector,
      {
        onVisibilityChanged: (visible) =>
          visible ? session.resume() : session.pause(),
      },
      m(
        Page,

        // Title bar with status and actions (always shown).
        this.renderTitleBar(attrs),
        m(PreviewBanner, {app: attrs.app}),

        // Profile page or dashboard content.
        session.profile !== undefined
          ? m(LiveProfileView, attrs)
          : this.renderDashboard(attrs),
      ),
    );
  }

  private renderTitleBar(attrs: DashboardAttrs): m.Children {
    return m(
      '.pf-memscope-title-bar',
      // Left: title + session pill (device + snapshot counter + pause/play).
      m(
        '.pf-memscope-title-bar__left',
        m('h1', 'Sysmem'),
        attrs.session.profile === undefined && this.renderTabStrip(),
      ),
      m(SessionPill, attrs),
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
}
