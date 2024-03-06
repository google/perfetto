// Copyright (C) 2024 The Android Open Source Project
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

import {
  FlowEventsAreaSelectedPanel,
  FlowEventsPanel,
} from '../../frontend/flow_events_panel';
import {globals} from '../../frontend/globals';
import {
  Plugin,
  PluginContext,
  PluginContextTrace,
  PluginDescriptor,
} from '../../public';

class FlowsPlugin implements Plugin {
  onActivate(_ctx: PluginContext): void {}

  async onTraceLoad(ctx: PluginContextTrace): Promise<void> {
    const tabUri = 'perfetto.Flows#FlowEvents';
    ctx.registerTab({
      isEphemeral: false,
      uri: tabUri,
      content: {
        render: () => {
          const selection = globals.state.currentSelection;
          if (selection?.kind === 'AREA') {
            return m(FlowEventsAreaSelectedPanel);
          } else {
            return m(FlowEventsPanel);
          }
        },
        getTitle: () => 'Flow Events',
      },
    });

    ctx.registerCommand({
      id: 'perfetto.Flows#ShowFlowsTab',
      name: `Show Flows Tab`,
      callback: () => {
        ctx.tabs.showTab(tabUri);
      },
    });
  }
}

export const plugin: PluginDescriptor = {
  pluginId: 'perfetto.Flows',
  plugin: FlowsPlugin,
};
