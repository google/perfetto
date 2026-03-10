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
import {Icons} from '../../../base/semantic_icons';
import {Tabs, TabsTab} from '../../../widgets/tabs';
import {renderWidgetShowcase} from '../widgets_page_utils';

const defaultTitles: Record<string, string> = {
  tab1: 'First Tab',
  tab2: 'Second Tab',
  tab3: 'Third Tab',
};

// Mutable state for renamed tab titles, persisted across redraws.
const tabTitles = new Map<string, string>(Object.entries(defaultTitles));

export function renderTabs(): m.Children {
  return [
    m(
      '.pf-widget-intro',
      m('h1', 'Tabs'),
      m(
        'p',
        'A simple tab bar widget with tab handles and gated content. ' +
          'Supports both controlled and uncontrolled modes, with optional ' +
          'close buttons and inline rename on double-click.',
      ),
    ),
    renderWidgetShowcase({
      renderWidget: (opts) => {
        const tabs: TabsTab[] = [
          {
            key: 'tab1',
            title: tabTitles.get('tab1') ?? defaultTitles['tab1'],
            leftIcon: opts.showIcons ? Icons.Info : undefined,
            content: m(
              '',
              {style: {padding: '16px'}},
              'Content for the first tab. This content is only rendered when the tab is active.',
            ),
          },
          {
            key: 'tab2',
            title: tabTitles.get('tab2') ?? defaultTitles['tab2'],
            leftIcon: opts.showIcons ? Icons.Chart : undefined,
            content: m(
              '',
              {style: {padding: '16px'}},
              'Content for the second tab. Switch between tabs to see the content change.',
            ),
            closeButton: opts.closeButton,
          },
          {
            key: 'tab3',
            title: tabTitles.get('tab3') ?? defaultTitles['tab3'],
            leftIcon: opts.showIcons ? Icons.Search : undefined,
            content: m(
              '',
              {style: {padding: '16px'}},
              'Content for the third tab. The tab bar uses the Gate component to efficiently manage content visibility.',
            ),
            closeButton: opts.closeButton,
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
            onTabClose: opts.closeButton
              ? (key) => {
                  console.log(`Close tab: ${key}`);
                }
              : undefined,
            onTabRename: opts.renamable
              ? (key, newTitle) => {
                  tabTitles.set(key, newTitle);
                }
              : undefined,
          }),
        );
      },
      initialOpts: {
        closeButton: false,
        showIcons: true,
        renamable: true,
      },
    }),
  ];
}
