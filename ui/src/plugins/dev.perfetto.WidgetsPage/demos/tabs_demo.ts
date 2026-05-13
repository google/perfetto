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
import {Button} from '../../../widgets/button';
import {MenuItem} from '../../../widgets/menu';
import {renderWidgetShowcase} from '../widgets_page_utils';
import {shortUuid} from '../../../base/uuid';

interface TabEntry {
  key: string;
  title: string;
  icon: string;
  content: string;
}

const defaultTabEntries: TabEntry[] = [
  {
    key: 'tab1',
    title: 'First Tab',
    icon: Icons.Info,
    content:
      'Content for the first tab. This content is only rendered when the tab is active.',
  },
  {
    key: 'tab2',
    title: 'Second Tab',
    icon: Icons.Chart,
    content:
      'Content for the second tab. Switch between tabs to see the content change.',
  },
  {
    key: 'tab3',
    title: 'Third Tab',
    icon: Icons.Search,
    content:
      'Content for the third tab. The tab bar uses the Gate component to efficiently manage content visibility.',
  },
];

// Mutable state persisted across redraws.
const tabEntries = [...defaultTabEntries];
let activeTabKey: string | undefined;

export function renderTabs(): m.Children {
  return [
    m(
      '.pf-widget-intro',
      m('h1', 'Tabs'),
      m(
        'p',
        'A simple tab bar widget with tab handles and gated content. ' +
          'Supports both controlled and uncontrolled modes, with optional ' +
          'close buttons, inline rename on double-click, and a new tab button.',
      ),
    ),
    renderWidgetShowcase({
      renderWidget: (opts) => {
        const makeMenuItems = (key: string) =>
          opts.menuItems
            ? [
                m(MenuItem, {
                  label: 'Action 1',
                  onclick: () => console.log(`Action 1: ${key}`),
                }),
                m(MenuItem, {
                  label: 'Action 2',
                  onclick: () => console.log(`Action 2: ${key}`),
                }),
              ]
            : undefined;

        const tabs: TabsTab[] = tabEntries.map((entry) => ({
          key: entry.key,
          title: entry.title,
          leftIcon: opts.showIcons ? entry.icon : undefined,
          content: m('', {style: {padding: '16px'}}, entry.content),
          closeButton: opts.closeButton && tabEntries.length > 1,
          menuItems: makeMenuItems(entry.key),
        }));

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
            activeTabKey,
            reorderable: opts.reorderable,
            onTabChange: (key) => {
              activeTabKey = key;
            },
            onTabClose:
              opts.closeButton && tabEntries.length > 1
                ? (key) => {
                    const idx = tabEntries.findIndex((e) => e.key === key);
                    if (idx === -1) return;
                    tabEntries.splice(idx, 1);
                    if (activeTabKey === key) {
                      // Switch to an adjacent tab.
                      activeTabKey =
                        tabEntries[Math.min(idx, tabEntries.length - 1)]?.key;
                    }
                  }
                : undefined,
            onTabRename: opts.renamable
              ? (key, newTitle) => {
                  const entry = tabEntries.find((e) => e.key === key);
                  if (entry) entry.title = newTitle;
                }
              : undefined,
            onTabReorder: opts.reorderable
              ? (draggedKey, beforeKey) => {
                  const draggedIdx = tabEntries.findIndex(
                    (e) => e.key === draggedKey,
                  );
                  if (draggedIdx === -1) return;
                  const [dragged] = tabEntries.splice(draggedIdx, 1);
                  if (beforeKey === undefined) {
                    tabEntries.push(dragged);
                  } else {
                    const beforeIdx = tabEntries.findIndex(
                      (e) => e.key === beforeKey,
                    );
                    tabEntries.splice(
                      beforeIdx === -1 ? tabEntries.length : beforeIdx,
                      0,
                      dragged,
                    );
                  }
                }
              : undefined,
            onNewTab: opts.newTabButton
              ? () => {
                  const id = shortUuid();
                  tabEntries.push({
                    key: id,
                    title: 'New Tab',
                    icon: Icons.Star,
                    content: `Content for dynamically added tab.`,
                  });
                  activeTabKey = id;
                }
              : undefined,
            rightContent: opts.rightButton
              ? m(Button, {icon: Icons.Filter, label: 'Filter'})
              : undefined,
          }),
        );
      },
      initialOpts: {
        closeButton: true,
        newTabButton: true,
        showIcons: true,
        renamable: true,
        reorderable: true,
        menuItems: true,
        rightButton: false,
      },
    }),
  ];
}
