// Copyright (C) 2025 The Android Open Source Project
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

import {PerfettoPlugin} from '../../public/plugin';
import {Trace} from '../../public/trace';

export default class implements PerfettoPlugin {
  static readonly id = 'com.example.Tabs';
  static readonly description = 'Example plugin to show how to register tabs.';

  async onTraceLoad(trace: Trace) {
    this.createPersistentTab(trace);
    this.createEphemeralTab(trace);
  }

  private createPersistentTab(trace: Trace) {
    // Register persistent tab - this tab is shown in the triple dot menu, and
    // can be opened and closed by the user or programmatically via showTab()
    // and hideTab().
    trace.tabs.registerTab({
      uri: 'com.example.Tabs#PersistentTab',
      isEphemeral: false,
      content: {
        getTitle: () => 'Example Persistent Tab',
        render: () => m('div', 'Hello from the global example tab!'),
      },
    });

    // Optional: Show the persistent tab right away
    // trace.tabs.showTab('com.example.Tabs#PersistentTab');

    // Or hide the tab later
    // trace.tabs.hideTab('com.example.Tabs#PersistentTab');
  }

  private createEphemeralTab(trace: Trace) {
    // Register an ephemeral tab - the only difference between an ephemeral tab
    // and a persistent tab is that the persistent tab is shown in the tab
    // drawer triple dot dropdown menu, and ephemeral tabs are not.
    trace.tabs.registerTab({
      uri: 'com.example.Tabs#EphemeralTab',
      isEphemeral: true,
      content: {
        getTitle: () => 'Example Ephemeral Tab',
        render: () => m('div', 'Hello from the ephemeral example tab!'),
      },
    });

    // Show the ephemeral tab right away
    trace.tabs.showTab('com.example.Tabs#EphemeralTab');
  }
}
