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
import {DrawerPanel, DrawerTab} from '../../../widgets/drawer_panel';
import {renderWidgetShowcase} from '../widgets_page_utils';

export function renderDrawerPanel(): m.Children {
  return [
    m(
      '.pf-widget-intro',
      m('h1', 'DrawerPanel'),
      m(
        'p',
        'A container with a main content area and a collapsible drawer at the bottom. ',
        'Supports two modes: simple (single drawer content) or tabs (multiple switchable tabs).',
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
            leftHandleContent: opts.leftContent && m(Button, {icon: 'Menu'}),
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
        leftContent: true,
      },
    }),
    m('h2', 'Tabs Mode'),
    m('p', 'Pass a tabs array for multiple switchable tabs with content.'),
    renderWidgetShowcase({
      renderWidget: (opts) => {
        const tabs: DrawerTab[] = [
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
            closable: opts.closable,
          },
          {
            key: 'tab3',
            title: 'Third Tab',
            content: m(
              'div',
              {style: {padding: '16px'}},
              'Content for the third tab',
            ),
            closable: opts.closable,
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
          m(DrawerPanel, {
            leftHandleContent: opts.leftContent && m(Button, {icon: 'Menu'}),
            mainContent: m('div', {style: {padding: '16px'}}, 'Main Content'),
            tabs,
            onTabChange: (key) => console.log('Tab changed to:', key),
            onTabClose: (key) => console.log('Tab closed:', key),
          }),
        );
      },
      initialOpts: {
        leftContent: true,
        closable: true,
      },
    }),
  ];
}
