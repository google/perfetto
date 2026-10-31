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
import {ProcessGraphPanel, ProcessGraphSession} from './process_graph_panel';

const PLUGIN_ID = 'com.android.ProcessGraph';
const TAB_URI = 'com.android.ProcessGraph#ProcessGraph';

export default class ProcessGraphPlugin implements PerfettoPlugin {
  static readonly id = PLUGIN_ID;
  static readonly description =
    'Visualizes Android service and content provider dependencies between ' +
    'processes layered by framework process state over time.';

  async onTraceLoad(ctx: Trace): Promise<void> {
    const res = await ctx.engine.query(`
      SELECT 1
      FROM slice
      WHERE category IN ('service_state', 'provider_state')
        AND name IN (
          'service_binding',
          'service_connection_added',
          'service_connection_removed',
          'provider_acquired',
          'provider_released'
        )
      LIMIT 1
    `);
    if (res.numRows() === 0) {
      return;
    }

    const session = new ProcessGraphSession();
    ctx.trash.defer(() => session.dispose());

    ctx.sidePanel.registerTab({
      uri: TAB_URI,
      title: 'Process Graph',
      icon: 'account_tree',
      render: () => m(ProcessGraphPanel, {trace: ctx, session}),
    });

    ctx.commands.registerCommand({
      id: 'com.android.ShowProcessGraph',
      name: 'Show Android Process Graph',
      callback: () => ctx.sidePanel.showTab(TAB_URI),
    });

    ctx.sidePanel.showTab(TAB_URI);
  }
}
