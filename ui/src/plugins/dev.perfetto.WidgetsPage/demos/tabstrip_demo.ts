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
import {TabStrip} from '../../../widgets/tabs';
import {renderWidgetShowcase} from '../widgets_page_utils';

let currentTab: string = 'foo';

export function renderTabStrip(): m.Children {
  return [
    m(
      '.pf-widget-intro',
      m('h1', 'TabStrip'),
      m(
        'p',
        'A horizontal tab navigation component for switching between different views or sections.',
      ),
    ),
    renderWidgetShowcase({
      renderWidget: () => {
        return m(TabStrip, {
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
  ];
}
