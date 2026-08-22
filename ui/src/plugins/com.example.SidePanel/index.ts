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
import type {PerfettoPlugin} from '../../public/plugin';
import type {Trace} from '../../public/trace';
import {Button, ButtonVariant} from '../../widgets/button';
import './side_panel_example.scss';

export default class implements PerfettoPlugin {
  static readonly id = 'com.example.SidePanel';
  static readonly description =
    'Example plugin showing how to register side panel tabs.';

  async onTraceLoad(trace: Trace): Promise<void> {
    trace.sidePanel.registerTab({
      uri: 'com.example.SidePanel#Hello',
      title: 'Example Side Panel',
      icon: 'info',
      render: () =>
        m(
          '.pf-side-panel-example',
          m('h2', 'Hello from the example side panel!'),
          m(
            'p',
            'This tab was contributed by the com.example.SidePanel plugin ',
            'via trace.sidePanel.registerTab().',
          ),
        ),
    });

    let count = 0;
    trace.sidePanel.registerTab({
      uri: 'com.example.SidePanel#Counter',
      title: 'Counter',
      icon: 'add_circle',
      render: () => {
        return m(
          '.pf-side-panel-example',
          m('h2', 'Counter tab'),
          m('p', `Count: ${count}`),
          m(Button, {
            variant: ButtonVariant.Filled,
            onclick: () => count++,
            label: 'Increment',
          }),
        );
      },
    });

    trace.commands.registerCommand({
      id: 'com.example.SidePanel#ShowHello',
      name: 'Example: Show hello side panel',
      callback: () => trace.sidePanel.showTab('com.example.SidePanel#Hello'),
    });

    trace.commands.registerCommand({
      id: 'com.example.SidePanel#ShowCounter',
      name: 'Example: Show counter side panel',
      callback: () => trace.sidePanel.showTab('com.example.SidePanel#Counter'),
    });

    // Open the tab right away so the example is discoverable.
    trace.sidePanel.showTab('com.example.SidePanel#Hello');
  }
}
