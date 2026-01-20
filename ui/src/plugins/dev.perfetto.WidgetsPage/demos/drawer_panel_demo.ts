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
import {Button} from '../../../widgets/button';
import {DrawerPanel} from '../../../widgets/drawer_panel';
import {Tabs, TabsTab} from '../../../widgets/tabs';
import {renderWidgetShowcase} from '../widgets_page_utils';

export function renderDrawerPanel(): m.Children {
  return [
    m(
      '.pf-widget-intro',
      m('h1', 'DrawerPanel'),
      m(
        'p',
        'A container with a main content area and a collapsible drawer at the bottom.',
      ),
    ),
    m('h2', 'Simple Mode'),
    m('p', 'Pass drawerContent for a simple drawer without tabs.'),
    renderWidgetShowcase({
      renderWidget: (opts) => {
        return m(
          '',
          {
            style: {
              height: '300px',
              width: '400px',
              border: 'solid 2px gray',
            },
          },
          m(DrawerPanel, {
            handleContent: opts.handleContent && m(Button, {icon: 'Menu'}),
            mainContent: m('div', {style: {padding: '16px'}}, 'Main Content'),
            drawerContent: m(
              'div',
              {style: {padding: '16px'}},
              'Drawer Content',
            ),
          }),
        );
      },
      initialOpts: {
        handleContent: true,
      },
    }),
    m('h2', 'With Tabs'),
    m(
      'p',
      'Combine with Tabs component using render prop for tabbed drawer content.',
    ),
    renderWidgetShowcase({
      renderWidget: (opts) => {
        const tabs: TabsTab[] = [
          {
            key: 'tab1',
            title: 'First Tab',
            content: m(
              'div',
              {style: {padding: '16px'}},
              'Content for the first tab',
            ),
          },
          {
            key: 'tab2',
            title: 'Second Tab',
            content: m(
              'div',
              {style: {padding: '16px'}},
              'Content for the second tab',
            ),
            closeButton: opts.closeButton,
          },
          {
            key: 'tab3',
            title: 'Third Tab',
            content: m(
              'div',
              {style: {padding: '16px'}},
              'Content for the third tab',
            ),
            closeButton: opts.closeButton,
          },
        ];

        return m(
          '',
          {
            style: {
              height: '300px',
              width: '500px',
              border: 'solid 2px gray',
            },
          },
          m(Tabs, {
            tabs,
            onTabChange: (key) => console.log('Tab changed to:', key),
            onTabClose: (key) => console.log('Tab closed:', key),
            render: ({handles, content}) =>
              m(DrawerPanel, {
                handleContent: [
                  opts.handleContent && m(Button, {icon: 'Menu'}),
                  m('.pf-tabs__tabs', handles),
                ],
                mainContent: m(
                  'div',
                  {style: {padding: '16px'}},
                  'Main Content',
                ),
                drawerContent: m('.pf-tabs__content', content),
              }),
          }),
        );
      },
      initialOpts: {
        handleContent: true,
        closeButton: true,
      },
    }),
  ];
}
