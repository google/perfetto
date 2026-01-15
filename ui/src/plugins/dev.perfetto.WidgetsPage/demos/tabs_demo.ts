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
import {Tabs, TabsTab} from '../../../widgets/tabs';
import {renderWidgetShowcase} from '../widgets_page_utils';

export function renderTabs(): m.Children {
  return [
    m(
      '.pf-widget-intro',
      m('h1', 'TabBar'),
      m(
        'p',
        'A simple tab bar widget with tab handles and gated content. ' +
          'Supports both controlled and uncontrolled modes, with optional close buttons on tabs.',
      ),
    ),
    renderWidgetShowcase({
      renderWidget: (opts) => {
        const tabs: TabsTab[] = [
          {
            key: 'tab1',
            title: 'First Tab',
            content: m(
              '',
              {style: {padding: '16px'}},
              'Content for the first tab. This content is only rendered when the tab is active.',
            ),
          },
          {
            key: 'tab2',
            title: 'Second Tab',
            content: m(
              '',
              {style: {padding: '16px'}},
              'Content for the second tab. Switch between tabs to see the content change.',
            ),
            closable: opts.closable,
          },
          {
            key: 'tab3',
            title: 'Third Tab',
            content: m(
              '',
              {style: {padding: '16px'}},
              'Content for the third tab. The tab bar uses the Gate component to efficiently manage content visibility.',
            ),
            closable: opts.closable,
          },
        ];

        return m(
          '',
          {
            style: {
              height: '200px',
              width: '500px',
              border: '1px solid var(--pf-color-border)',
            },
          },
          m(Tabs, {
            tabs,
            onTabClose: opts.closable
              ? (key) => {
                  console.log(`Close tab: ${key}`);
                }
              : undefined,
          }),
        );
      },
      initialOpts: {
        closable: false,
      },
    }),
  ];
}
