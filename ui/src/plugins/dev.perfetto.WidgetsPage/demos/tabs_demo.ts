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
import {Tabs} from '../../../widgets/tabs';
import {renderWidgetShowcase} from '../widgets_page_utils';

let currentTab: string = 'foo';

export function renderTabs(): m.Children {
  return [
    m(
      '.pf-widget-intro',
      m('h1', 'Tabs'),
      m(
        'p',
        'A horizontal tab navigation component for switching between different views or sections. Can optionally contain tab content.',
      ),
    ),
    m('h2', 'Basic Tabs'),
    renderWidgetShowcase({
      renderWidget: () => {
        return m(Tabs, {
          tabs: [
            {key: 'foo', title: 'Foo'},
            {key: 'bar', title: 'Bar'},
            {key: 'baz', title: 'Baz'},
          ],
          currentTabKey: currentTab,
          onTabChange: (key) => {
            currentTab = key;
          },
        });
      },
      initialOpts: {},
    }),
    m('h2', 'Tabs with Content'),
    renderWidgetShowcase({
      renderWidget: () => {
        return m(Tabs, {
          tabs: [
            {key: 'foo', title: 'Foo', content: m('p', 'Content for Foo tab')},
            {key: 'bar', title: 'Bar', content: m('p', 'Content for Bar tab')},
            {key: 'baz', title: 'Baz', content: m('p', 'Content for Baz tab')},
          ],
          currentTabKey: currentTab,
          onTabChange: (key) => {
            currentTab = key;
          },
        });
      },
      initialOpts: {},
    }),
  ];
}
