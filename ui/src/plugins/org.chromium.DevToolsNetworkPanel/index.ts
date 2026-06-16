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

import './styles.scss';
import m from 'mithril';
import type {PerfettoPlugin} from '../../public/plugin';
import type {Trace} from '../../public/trace';
import {Icon} from '../../widgets/icon';
import {NetworkPanelView} from './network_panel';

export default class NetworkRequestsPlugin implements PerfettoPlugin {
  static readonly id = 'org.chromium.NetworkRequests';
  static readonly description = 'A network exploration panel';

  async onTraceLoad(trace: Trace): Promise<void> {
    const tabUri = `${NetworkRequestsPlugin.id}#NetworkTab`;

    trace.commands.registerCommand({
      id: 'org.chromium.NetworkRequests.ShowNetworkTab',
      name: 'Show Network Tab',
      callback: () => {
        trace.tabs.showTab(tabUri);
      },
    });

    trace.tabs.registerTab({
      isEphemeral: false,
      uri: tabUri,
      content: {
        getTitle: () => m(
          'span',
          {style: {display: 'flex', alignItems: 'center', gap: '6px'}},
          m(Icon, {icon: 'signal_cellular_alt'}),
          'Network'
        ) as unknown as string,
        render: () => m(NetworkPanelView, {trace}),
      },
    });

    const result = await trace.engine.query(`
      SELECT 1 FROM slice 
      WHERE category LIKE '%devtools.timeline%' 
         OR category LIKE '%netlog%' 
         OR category LIKE '%net%' 
         OR category LIKE '%network%' 
      LIMIT 1
    `);
    if (result.numRows() > 0) {
      trace.tabs.addDefaultTab(tabUri);
    }
  }
}
